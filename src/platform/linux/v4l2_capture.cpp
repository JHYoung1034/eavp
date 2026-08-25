#include "eavp/platform/linux/v4l2_capture.hpp"

#include <cerrno>
#include <cstring>
#include <limits>
#include <linux/videodev2.h>
#include <memory>
#include <new>
#include <string>
#include <time.h>
#include <utility>

#include "eavp/base/time.hpp"
#include "eavp/management/metrics.hpp"
#include "platform/linux/v4l2_api.hpp"
#include "platform/linux/v4l2_capture_internal.hpp"

namespace eavp {
namespace {

const std::int64_t kMicrosecondsPerSecond = 1000000;
const std::uint32_t kSequenceHalfRange = 0x80000000U;

Status allocation_failure() {
    return Status(StatusCode::kResourceExhausted);
}

Status unexpected_failure() {
    return Status(StatusCode::kInternal);
}

Status v4l2_failure(StatusCode code, const char* message,
                    const char* operation, int native_code) {
    return Status(code, message, "v4l2", operation, native_code);
}

bool checked_add(std::size_t left, std::size_t right,
                 std::size_t* result) {
    if (result == NULL ||
        left > std::numeric_limits<std::size_t>::max() - right) {
        return false;
    }
    *result = left + right;
    return true;
}

bool checked_multiply(std::size_t left, std::size_t right,
                      std::size_t* result) {
    if (result == NULL ||
        (left != 0U &&
         right > std::numeric_limits<std::size_t>::max() / left)) {
        return false;
    }
    *result = left * right;
    return true;
}

template <typename ObserverCall>
Status invoke_observer(const ObserverCall& call) {
    try {
        return call();
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    } catch (...) {
        return unexpected_failure();
    }
}

void remember_first(Status* first, const Status& candidate) {
    if (first->ok() && !candidate.ok()) *first = candidate;
}

class BufferFrameAllocator : public detail::V4L2FrameAllocator {
public:
    Result<Buffer> allocate(
        std::size_t capacity,
        const std::vector<PlaneLayout>& planes) override {
        return Buffer::allocate(capacity, planes);
    }
};

class RegistryV4L2Observer : public detail::V4L2Observer {
public:
    explicit RegistryV4L2Observer(MetricRegistry* metrics)
        : metrics_(metrics) {}

    Status on_captured(std::size_t copied_bytes) override {
        if (metrics_ == NULL) return Status::ok_status();
        Status result = metrics_->increment_counter("v4l2.frames.captured");
        remember_first(&result, metrics_->increment_counter(
            "v4l2.bytes.copied", static_cast<std::uint64_t>(copied_bytes)));
        return result;
    }

    Status on_would_block() override {
        return metrics_ == NULL
            ? Status::ok_status()
            : metrics_->increment_counter("v4l2.dequeue.would_block");
    }

    Status on_pending(bool pending) override {
        return metrics_ == NULL
            ? Status::ok_status()
            : metrics_->set_gauge("v4l2.pending_frame", pending ? 1.0 : 0.0);
    }

    Status on_dropped_on_stop() override {
        return metrics_ == NULL
            ? Status::ok_status()
            : metrics_->increment_counter("v4l2.frames.dropped_on_stop");
    }

    Status on_sequence_gap(std::uint32_t missing_frames) override {
        return metrics_ == NULL
            ? Status::ok_status()
            : metrics_->increment_counter(
                  "v4l2.sequence.gaps",
                  static_cast<std::uint64_t>(missing_frames));
    }

    Status on_fatal(const Status&) override {
        return Status::ok_status();
    }

private:
    MetricRegistry* metrics_;
};

struct TestDependencies {
    TestDependencies(std::unique_ptr<detail::V4L2System> system_value,
                     std::unique_ptr<detail::V4L2FrameAllocator> allocator_value,
                     detail::V4L2Observer* observer_value)
        : system(std::move(system_value)), allocator(std::move(allocator_value)),
          observer(observer_value) {}

    std::unique_ptr<detail::V4L2System> system;
    std::unique_ptr<detail::V4L2FrameAllocator> allocator;
    detail::V4L2Observer* observer;
};

thread_local TestDependencies* current_test_dependencies = NULL;

class ScopedTestDependencies {
public:
    explicit ScopedTestDependencies(TestDependencies* dependencies)
        : previous_(current_test_dependencies) {
        current_test_dependencies = dependencies;
    }

    ~ScopedTestDependencies() {
        current_test_dependencies = previous_;
    }

private:
    TestDependencies* previous_;
};

bool timeval_to_us(const struct timeval& timestamp, std::int64_t* result) {
    if (result == NULL || timestamp.tv_sec < 0 || timestamp.tv_usec < 0 ||
        timestamp.tv_usec >= kMicrosecondsPerSecond ||
        (timestamp.tv_sec == 0 && timestamp.tv_usec == 0)) {
        return false;
    }
    const std::uint64_t seconds =
        static_cast<std::uint64_t>(timestamp.tv_sec);
    const std::uint64_t microseconds =
        static_cast<std::uint64_t>(timestamp.tv_usec);
    const std::uint64_t maximum =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (seconds > maximum / static_cast<std::uint64_t>(kMicrosecondsPerSecond)) {
        return false;
    }
    const std::uint64_t seconds_us =
        seconds * static_cast<std::uint64_t>(kMicrosecondsPerSecond);
    if (microseconds > maximum - seconds_us) return false;
    *result = static_cast<std::int64_t>(seconds_us + microseconds);
    return true;
}

bool timespec_to_us(const struct timespec& timestamp, std::int64_t* result) {
    if (result == NULL || timestamp.tv_sec < 0 || timestamp.tv_nsec < 0 ||
        timestamp.tv_nsec >= 1000000000L) {
        return false;
    }
    const std::uint64_t seconds =
        static_cast<std::uint64_t>(timestamp.tv_sec);
    const std::uint64_t microseconds =
        static_cast<std::uint64_t>(timestamp.tv_nsec / 1000L);
    const std::uint64_t maximum =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (seconds > maximum / static_cast<std::uint64_t>(kMicrosecondsPerSecond)) {
        return false;
    }
    const std::uint64_t seconds_us =
        seconds * static_cast<std::uint64_t>(kMicrosecondsPerSecond);
    if (microseconds > maximum - seconds_us) return false;
    *result = static_cast<std::int64_t>(seconds_us + microseconds);
    return true;
}

}  // namespace

class V4L2SourceNode::Impl {
public:
    Impl(const V4L2CaptureConfig& config_value, MetricRegistry* metrics_value,
         std::unique_ptr<detail::V4L2System> system_value,
         std::unique_ptr<detail::V4L2FrameAllocator> allocator_value,
         detail::V4L2Observer* observer_value)
        : config(config_value), metrics(metrics_value),
          system(std::move(system_value)), allocator(std::move(allocator_value)),
          output("video_output"), actual_format(), cpu_format(), pending(),
          has_pts(false), last_pts(0), has_sequence(false), expected_sequence(0U),
          owned_observer(), observer(observer_value) {
        if (observer == NULL) {
            owned_observer.reset(new RegistryV4L2Observer(metrics));
            observer = owned_observer.get();
        }
    }

    Status report_media_failure(const Status& media_status) {
        invoke_observer([this, &media_status]() {
            return observer->on_fatal(media_status);
        });
        return media_status;
    }

    Status report_would_block(const Status& media_status) {
        const Status observer_status = invoke_observer([this]() {
            return observer->on_would_block();
        });
        return observer_status.ok() ? media_status : observer_status;
    }

    Status timestamp_for(const detail::V4L2DequeuedBuffer& dequeued,
                         std::int64_t* pts) const {
        const bool monotonic =
            (dequeued.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) ==
            V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
        std::int64_t candidate = 0;
        if (!monotonic || !timeval_to_us(dequeued.timestamp, &candidate)) {
            struct timespec now;
            std::memset(&now, 0, sizeof(now));
            if (::clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
                const int native_code = errno;
                return v4l2_failure(StatusCode::kIoError,
                                    std::strerror(native_code),
                                    "clock_gettime(CLOCK_MONOTONIC)",
                                    native_code);
            }
            if (!timespec_to_us(now, &candidate)) {
                return Status(StatusCode::kCorruptData,
                              "V4L2 monotonic timestamp is invalid or overflows");
            }
        }
        if (has_pts && candidate < last_pts) candidate = last_pts;
        *pts = candidate;
        return Status::ok_status();
    }

    Status sequence_for(std::uint32_t sequence,
                        std::uint32_t* missing_frames) const {
        *missing_frames = 0U;
        if (!has_sequence) return Status::ok_status();
        const std::uint32_t delta = sequence - expected_sequence;
        if (delta == 0U) return Status::ok_status();
        if (delta < kSequenceHalfRange) {
            *missing_frames = delta;
            return Status::ok_status();
        }
        return Status(StatusCode::kCorruptData,
                      "V4L2 buffer sequence moved backwards");
    }

    std::size_t rows_for_plane(std::size_t plane_index) const {
        if (config.pixel_format() == PixelFormat::kYuyv422 ||
            plane_index == 0U) {
            return static_cast<std::size_t>(config.height());
        }
        return static_cast<std::size_t>(config.height() / 2);
    }

    Status validate_copy_range(
        const detail::V4L2DequeuedBuffer& dequeued,
        const PlaneLayout& plane, std::size_t source_offset,
        std::size_t visible_row_bytes, std::size_t rows,
        std::size_t target_size, std::size_t target_capacity,
        std::size_t* source_last_end,
        std::size_t* target_last_end) const {
        if (rows == 0U || visible_row_bytes == 0U ||
            visible_row_bytes > plane.stride ||
            dequeued.bytesused > dequeued.mapped_length) {
            return Status(StatusCode::kCorruptData,
                          "V4L2 dequeued plane metadata is invalid");
        }
        std::size_t last_row_offset = 0U;
        std::size_t source_last_row = 0U;
        std::size_t target_last_row = 0U;
        std::size_t target_absolute_end = 0U;
        if (!checked_multiply(plane.stride, rows - 1U, &last_row_offset) ||
            !checked_add(source_offset, last_row_offset, &source_last_row) ||
            !checked_add(source_last_row, visible_row_bytes, source_last_end) ||
            !checked_add(last_row_offset, visible_row_bytes, target_last_end) ||
            !checked_add(plane.offset, last_row_offset, &target_last_row) ||
            !checked_add(target_last_row, visible_row_bytes,
                         &target_absolute_end)) {
            return Status(StatusCode::kCorruptData,
                          "V4L2 dequeued plane range overflows");
        }
        if (*source_last_end > static_cast<std::size_t>(dequeued.bytesused) ||
            *source_last_end > dequeued.mapped_length ||
            *target_last_end > target_size ||
            target_absolute_end > target_capacity) {
            return Status(StatusCode::kCorruptData,
                          "V4L2 dequeued bytes do not cover visible pixels");
        }
        return Status::ok_status();
    }

    Status copy_frame(const detail::V4L2DequeuedBuffer& dequeued,
                      std::shared_ptr<const VideoFrame>* frame,
                      std::size_t* copied_bytes,
                      std::int64_t* pts,
                      std::uint32_t* missing_frames) {
        if ((dequeued.flags & V4L2_BUF_FLAG_ERROR) != 0U) {
            return Status(StatusCode::kCorruptData,
                          "V4L2 driver marked dequeued buffer as corrupt");
        }
        if (dequeued.data == NULL || actual_format.get() == NULL ||
            cpu_format.get() == NULL) {
            return Status(StatusCode::kInvalidState,
                          "V4L2 capture format or mapped data is unavailable");
        }

        Status status = timestamp_for(dequeued, pts);
        if (!status.ok()) return status;
        status = sequence_for(dequeued.sequence, missing_frames);
        if (!status.ok()) return status;

        const detail::V4L2NegotiatedFormat& negotiated = system->negotiated();
        const std::vector<PlaneLayout>& planes = cpu_format->planes();
        if (planes.size() != negotiated.visible_row_bytes.size() ||
            planes.size() != negotiated.source_offsets.size()) {
            return Status(StatusCode::kCorruptData,
                          "V4L2 negotiated plane metadata is inconsistent");
        }
        Result<Buffer> allocated =
            allocator->allocate(negotiated.total_capacity, planes);
        if (!allocated.ok()) return allocated.status();
        Buffer target = allocated.take_value();
        *copied_bytes = 0U;

        for (std::size_t plane_index = 0U;
             plane_index < planes.size(); ++plane_index) {
            Result<MappedRegion> mapped =
                target.map_plane(plane_index, MapMode::kReadWrite);
            if (!mapped.ok()) return mapped.status();
            MappedRegion region = mapped.take_value();
            std::uint8_t* const destination = region.mutable_data();
            if (destination == NULL) {
                return Status(StatusCode::kCorruptData,
                              "V4L2 target plane is not writable");
            }
            const PlaneLayout& plane = planes[plane_index];
            const std::size_t rows = rows_for_plane(plane_index);
            const std::size_t visible =
                negotiated.visible_row_bytes[plane_index];
            std::size_t source_end = 0U;
            std::size_t target_end = 0U;
            status = validate_copy_range(
                dequeued, plane, negotiated.source_offsets[plane_index],
                visible, rows, region.size(), negotiated.total_capacity,
                &source_end, &target_end);
            if (!status.ok()) return status;
            (void)source_end;
            (void)target_end;

            std::memset(destination, 0, region.size());
            for (std::size_t row = 0U; row < rows; ++row) {
                const std::size_t row_offset = row * plane.stride;
                const std::size_t source_position =
                    negotiated.source_offsets[plane_index] + row_offset;
                std::memcpy(destination + row_offset,
                            dequeued.data + source_position, visible);
            }
            std::size_t plane_copied = 0U;
            if (!checked_multiply(visible, rows, &plane_copied) ||
                !checked_add(*copied_bytes, plane_copied, copied_bytes)) {
                return Status(StatusCode::kCorruptData,
                              "V4L2 copied byte count overflows");
            }
        }

        const Result<VideoFrame> created = VideoFrame::create(
            target, *cpu_format, *pts,
            TimeBase::create(1, 1000000).value());
        if (!created.ok()) return created.status();
        try {
            frame->reset(new VideoFrame(created.value()));
        } catch (const std::bad_alloc&) {
            return allocation_failure();
        } catch (...) {
            return unexpected_failure();
        }
        return Status::ok_status();
    }

    V4L2CaptureConfig config;
    MetricRegistry* metrics;
    std::unique_ptr<detail::V4L2System> system;
    std::unique_ptr<detail::V4L2FrameAllocator> allocator;
    OutputPort<VideoFrame> output;
    std::unique_ptr<VideoFormat> actual_format;
    std::unique_ptr<VideoFormat> cpu_format;
    std::shared_ptr<const VideoFrame> pending;
    bool has_pts;
    std::int64_t last_pts;
    bool has_sequence;
    std::uint32_t expected_sequence;
    std::unique_ptr<detail::V4L2Observer> owned_observer;
    detail::V4L2Observer* observer;
};

V4L2SourceNode::V4L2SourceNode(const std::string& id,
                               std::unique_ptr<Impl> impl)
    : MediaNode(id), impl_(std::move(impl)) {}

V4L2SourceNode::~V4L2SourceNode() noexcept {}

Result<std::unique_ptr<V4L2SourceNode> > V4L2SourceNode::create(
    const std::string& id, const V4L2CaptureConfig& config,
    MetricRegistry* metrics) {
    try {
        if (id.empty()) {
            return Result<std::unique_ptr<V4L2SourceNode> >(Status(
                StatusCode::kInvalidArgument,
                "V4L2 source node id must not be empty"));
        }

        std::unique_ptr<detail::V4L2System> system;
        std::unique_ptr<detail::V4L2FrameAllocator> allocator;
        detail::V4L2Observer* observer = NULL;
        if (current_test_dependencies != NULL) {
            system = std::move(current_test_dependencies->system);
            allocator = std::move(current_test_dependencies->allocator);
            observer = current_test_dependencies->observer;
        } else {
            system.reset(new detail::V4L2System(
                detail::create_linux_v4l2_api()));
            allocator.reset(new BufferFrameAllocator());
        }
        if (!system || !allocator) {
            return Result<std::unique_ptr<V4L2SourceNode> >(Status(
                StatusCode::kInvalidArgument,
                "V4L2 source dependencies must be configured"));
        }
        std::unique_ptr<Impl> impl(new Impl(
            config, metrics, std::move(system), std::move(allocator), observer));
        return Result<std::unique_ptr<V4L2SourceNode> >(
            std::unique_ptr<V4L2SourceNode>(
                new V4L2SourceNode(id, std::move(impl))));
    } catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<V4L2SourceNode> >(allocation_failure());
    } catch (...) {
        return Result<std::unique_ptr<V4L2SourceNode> >(unexpected_failure());
    }
}

OutputPort<VideoFrame>& V4L2SourceNode::output() {
    return impl_->output;
}

Result<VideoFormat> V4L2SourceNode::actual_format() const {
    try {
        if (!impl_->actual_format) {
            return Result<VideoFormat>(Status(
                StatusCode::kInvalidState,
                "V4L2 actual format is unavailable before prepare"));
        }
        return Result<VideoFormat>(*impl_->actual_format);
    } catch (const std::bad_alloc&) {
        return Result<VideoFormat>(allocation_failure());
    } catch (...) {
        return Result<VideoFormat>(unexpected_failure());
    }
}

Result<std::vector<struct pollfd> > V4L2SourceNode::poll_descriptors() {
    try {
        if (state() != NodeState::kRunning) {
            return Result<std::vector<struct pollfd> >(Status(
                StatusCode::kInvalidState,
                "V4L2 wait source requires a running node"));
        }
        return impl_->system->poll_descriptors();
    } catch (const std::bad_alloc&) {
        return Result<std::vector<struct pollfd> >(allocation_failure());
    } catch (...) {
        return Result<std::vector<struct pollfd> >(unexpected_failure());
    }
}

Result<bool> V4L2SourceNode::evaluate_poll_events(
    const std::vector<struct pollfd>& descriptors) {
    try {
        if (state() != NodeState::kRunning) {
            return Result<bool>(Status(
                StatusCode::kInvalidState,
                "V4L2 poll evaluation requires a running node"));
        }
        return impl_->system->evaluate_poll_events(descriptors);
    } catch (const std::bad_alloc&) {
        return Result<bool>(allocation_failure());
    } catch (...) {
        return Result<bool>(unexpected_failure());
    }
}

Status V4L2SourceNode::on_prepare() {
    try {
        if (impl_->actual_format && impl_->cpu_format) {
            return Status::ok_status();
        }
        const Status prepare_status = impl_->system->prepare(impl_->config);
        if (!prepare_status.ok()) return prepare_status;
        const detail::V4L2NegotiatedFormat& negotiated =
            impl_->system->negotiated();
        const Result<VideoFormat> cpu = VideoFormat::create(
            negotiated.format.pixel_format(), negotiated.format.width(),
            negotiated.format.height(), MemoryDomain::kCpu,
            negotiated.format.planes(), negotiated.format.color_range(),
            negotiated.format.color_primaries(), negotiated.format.transfer(),
            negotiated.format.matrix());
        if (!cpu.ok()) return cpu.status();
        impl_->actual_format.reset(new VideoFormat(negotiated.format));
        impl_->cpu_format.reset(new VideoFormat(cpu.value()));
        return Status::ok_status();
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    } catch (...) {
        return unexpected_failure();
    }
}

Status V4L2SourceNode::on_start() {
    try {
        impl_->pending.reset();
        impl_->has_pts = false;
        impl_->last_pts = 0;
        impl_->has_sequence = false;
        impl_->expected_sequence = 0U;
        const Status start_status = impl_->system->start();
        if (!start_status.ok()) return start_status;
        return invoke_observer([this]() {
            return impl_->observer->on_pending(false);
        });
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    } catch (...) {
        return unexpected_failure();
    }
}

Status V4L2SourceNode::on_stop() {
    try {
        Status observer_status;
        if (impl_->pending) {
            impl_->pending.reset();
            remember_first(&observer_status, invoke_observer([this]() {
                return impl_->observer->on_dropped_on_stop();
            }));
            remember_first(&observer_status, invoke_observer([this]() {
                return impl_->observer->on_pending(false);
            }));
        }
        const Status stop_status = impl_->system->stop();
        if (!stop_status.ok()) return impl_->report_media_failure(stop_status);
        return observer_status;
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    } catch (...) {
        return unexpected_failure();
    }
}

Status V4L2SourceNode::on_reset() {
    try {
        Status observer_status;
        if (impl_->pending) {
            impl_->pending.reset();
            observer_status = invoke_observer([this]() {
                return impl_->observer->on_pending(false);
            });
        }
        const Status reset_status = impl_->system->reset();
        impl_->actual_format.reset();
        impl_->cpu_format.reset();
        impl_->has_pts = false;
        impl_->has_sequence = false;
        if (!reset_status.ok()) return impl_->report_media_failure(reset_status);
        return observer_status;
    } catch (const std::bad_alloc&) {
        impl_->actual_format.reset();
        impl_->cpu_format.reset();
        return allocation_failure();
    } catch (...) {
        impl_->actual_format.reset();
        impl_->cpu_format.reset();
        return unexpected_failure();
    }
}

Status V4L2SourceNode::on_tick() {
    try {
        if (impl_->pending) {
            const Status output_status = impl_->output.send(impl_->pending);
            if (!output_status.ok()) {
                if (output_status.code() == StatusCode::kWouldBlock) {
                    return impl_->report_would_block(output_status);
                }
                return impl_->report_media_failure(output_status);
            }
            impl_->pending.reset();
            return invoke_observer([this]() {
                return impl_->observer->on_pending(false);
            });
        }

        Result<detail::V4L2DequeuedBuffer> dequeued = impl_->system->dequeue();
        if (!dequeued.ok()) {
            if (dequeued.status().code() == StatusCode::kWouldBlock) {
                return impl_->report_would_block(dequeued.status());
            }
            return impl_->report_media_failure(dequeued.status());
        }
        const detail::V4L2DequeuedBuffer buffer = dequeued.value();
        Status media_status;
        std::shared_ptr<const VideoFrame> frame;
        std::size_t copied_bytes = 0U;
        std::int64_t pts = 0;
        std::uint32_t missing_frames = 0U;
        try {
            media_status = impl_->copy_frame(
                buffer, &frame, &copied_bytes, &pts, &missing_frames);
        } catch (const std::bad_alloc&) {
            media_status = allocation_failure();
        } catch (...) {
            media_status = unexpected_failure();
        }

        Status requeue_status;
        try {
            requeue_status = impl_->system->requeue(buffer.index);
        } catch (const std::bad_alloc&) {
            requeue_status = allocation_failure();
        } catch (...) {
            requeue_status = unexpected_failure();
        }
        if (!requeue_status.ok()) return requeue_status;
        if (!media_status.ok()) return impl_->report_media_failure(media_status);

        impl_->has_pts = true;
        impl_->last_pts = pts;
        impl_->has_sequence = true;
        impl_->expected_sequence = buffer.sequence + 1U;

        Status observer_status = invoke_observer([this, copied_bytes]() {
            return impl_->observer->on_captured(copied_bytes);
        });
        if (observer_status.ok() && missing_frames != 0U) {
            observer_status = invoke_observer([this, missing_frames]() {
                return impl_->observer->on_sequence_gap(missing_frames);
            });
        }
        if (!observer_status.ok()) return observer_status;

        const Status output_status = impl_->output.send(frame);
        if (!output_status.ok()) {
            if (output_status.code() == StatusCode::kWouldBlock) {
                impl_->pending = frame;
                Status pending_status = invoke_observer([this]() {
                    return impl_->observer->on_pending(true);
                });
                const Status would_block_status = impl_->report_would_block(
                    output_status);
                if (!pending_status.ok()) return pending_status;
                return would_block_status;
            }
            return impl_->report_media_failure(output_status);
        }
        return Status::ok_status();
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    } catch (...) {
        return unexpected_failure();
    }
}

namespace detail {

Result<std::unique_ptr<V4L2SourceNode> > V4L2SourceNodeTestPeer::create(
    const std::string& id, const V4L2CaptureConfig& config,
    MetricRegistry* metrics, std::unique_ptr<V4L2System> system,
    std::unique_ptr<V4L2FrameAllocator> allocator,
    V4L2Observer* observer) {
    try {
        if (id.empty() || !system || !allocator) {
            return Result<std::unique_ptr<V4L2SourceNode> >(Status(
                StatusCode::kInvalidArgument,
                "V4L2 source test dependencies must be configured"));
        }
        TestDependencies dependencies(
            std::move(system), std::move(allocator), observer);
        ScopedTestDependencies scoped(&dependencies);
        return V4L2SourceNode::create(id, config, metrics);
    } catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<V4L2SourceNode> >(allocation_failure());
    } catch (...) {
        return Result<std::unique_ptr<V4L2SourceNode> >(unexpected_failure());
    }
}

}  // namespace detail
}  // namespace eavp
