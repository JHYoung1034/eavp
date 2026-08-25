#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <linux/videodev2.h>
#include <limits>
#include <memory>
#include <new>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <time.h>
#include <type_traits>
#include <utility>
#include <vector>

#include "eavp/management/metrics.hpp"
#include "eavp/media/port.hpp"
#include "platform/linux/v4l2_capture_internal.hpp"
#include "support/fake_v4l2_api.hpp"

namespace {

std::int64_t timestamp_microseconds(long seconds, long microseconds) {
    return static_cast<std::int64_t>(seconds) * 1000000 +
           static_cast<std::int64_t>(microseconds);
}

struct SourceMemory {
    SourceMemory(std::size_t buffer_count, std::size_t capacity)
        : regions(buffer_count, std::vector<std::uint8_t>(capacity, 0xEEU)),
          next_map(0U) {}

    std::vector<std::vector<std::uint8_t> > regions;
    std::size_t next_map;
};

class MemoryV4L2Api : public eavp_test::FakeV4L2Api {
public:
    MemoryV4L2Api(const std::shared_ptr<eavp_test::FakeV4L2Trace>& trace,
                  const std::shared_ptr<SourceMemory>& memory)
        : FakeV4L2Api(trace), memory_(memory) {}

    void* map_memory(void*, std::size_t length, int, int, int,
                     std::int64_t) override {
        if (memory_->next_map >= memory_->regions.size() ||
            memory_->regions[memory_->next_map].size() < length) {
            return MAP_FAILED;
        }
        return memory_->regions[memory_->next_map++].data();
    }

    int unmap_memory(void*, std::size_t) override { return 0; }

    int close_device(int fd) override {
        memory_->next_map = 0U;
        return FakeV4L2Api::close_device(fd);
    }

private:
    std::shared_ptr<SourceMemory> memory_;
};

class TestStorage : public eavp::BufferStorage {
public:
    TestStorage(std::size_t capacity, eavp::MemoryDomain domain,
                const eavp::Status& map_status)
        : bytes_(capacity, 0xABU), domain_(domain), map_status_(map_status),
          provider_("v4l2-test") {}

    eavp::MemoryDomain memory_domain() const override { return domain_; }
    std::size_t capacity() const override { return bytes_.size(); }
    const std::string& provider_id() const override { return provider_; }
    eavp::Status map(eavp::MapMode, std::uint8_t** data,
                     std::size_t* size) override {
        if (!map_status_.ok()) return map_status_;
        *data = bytes_.data();
        *size = bytes_.size();
        return eavp::Status::ok_status();
    }
    eavp::Status unmap() override { return eavp::Status::ok_status(); }
    eavp::Result<eavp::NativeBufferHandle> export_dmabuf() const override {
        return eavp::Result<eavp::NativeBufferHandle>(
            eavp::Status(eavp::StatusCode::kUnsupported));
    }

private:
    std::vector<std::uint8_t> bytes_;
    eavp::MemoryDomain domain_;
    eavp::Status map_status_;
    std::string provider_;
};

class ScriptedAllocator : public eavp::detail::V4L2FrameAllocator {
public:
    enum Mode {
        kNormal,
        kAllocationFailure,
        kMapFailure,
        kWrongDomain,
        kBadAlloc,
        kUnexpected
    };

    explicit ScriptedAllocator(Mode mode = kNormal) : mode_(mode) {}

    eavp::Result<eavp::Buffer> allocate(
        std::size_t capacity,
        const std::vector<eavp::PlaneLayout>& planes) override {
        if (mode_ == kBadAlloc) throw std::bad_alloc();
        if (mode_ == kUnexpected) throw std::runtime_error("allocator threw");
        if (mode_ == kAllocationFailure) {
            return eavp::Result<eavp::Buffer>(eavp::Status(
                eavp::StatusCode::kResourceExhausted, "scripted allocation failure"));
        }
        if (mode_ == kNormal) return eavp::Buffer::allocate(capacity, planes);
        const eavp::Status map_status = mode_ == kMapFailure
            ? eavp::Status(eavp::StatusCode::kIoError,
                           "scripted map failure")
            : eavp::Status::ok_status();
        const eavp::MemoryDomain domain = mode_ == kWrongDomain
            ? eavp::MemoryDomain::kMmap : eavp::MemoryDomain::kCpu;
        std::shared_ptr<eavp::BufferStorage> storage(
            new TestStorage(capacity, domain, map_status));
        return eavp::Buffer::create(storage, planes);
    }

private:
    Mode mode_;
};

class FailOnceFormatFactory : public eavp::detail::V4L2FormatFactory {
public:
    enum Failure { kStatusFailure, kBadAlloc, kUnexpected };

    explicit FailOnceFormatFactory(Failure failure)
        : failure_(failure), failed_(false) {}

    eavp::Result<eavp::VideoFormat> create_cpu(
        const eavp::VideoFormat& negotiated) override {
        return eavp::VideoFormat::create(
            negotiated.pixel_format(), negotiated.width(), negotiated.height(),
            eavp::MemoryDomain::kCpu, negotiated.planes(),
            negotiated.color_range(), negotiated.color_primaries(),
            negotiated.transfer(), negotiated.matrix());
    }

    eavp::Result<eavp::VideoFormat> clone_actual(
        const eavp::VideoFormat& negotiated) override {
        if (!failed_) {
            failed_ = true;
            if (failure_ == kBadAlloc) throw std::bad_alloc();
            if (failure_ == kUnexpected) {
                throw std::runtime_error("format factory threw");
            }
            return eavp::Result<eavp::VideoFormat>(eavp::Status(
                eavp::StatusCode::kResourceExhausted,
                "scripted format failure"));
        }
        return eavp::Result<eavp::VideoFormat>(negotiated);
    }

private:
    Failure failure_;
    bool failed_;
};

class ScriptedClock : public eavp::detail::V4L2Clock {
public:
    enum Mode {
        kSuccess,
        kSyscallFailure,
        kInvalidTimespec,
        kBadAlloc,
        kUnexpected
    };

    explicit ScriptedClock(Mode mode = kSuccess)
        : mode_(mode), last_error_(0) {}

    int monotonic_now(struct timespec* value) override {
        if (mode_ == kBadAlloc) throw std::bad_alloc();
        if (mode_ == kUnexpected) throw std::runtime_error("clock threw");
        if (mode_ == kSyscallFailure) {
            last_error_ = EIO;
            return -1;
        }
        if (mode_ == kInvalidTimespec) {
            const time_t maximum_seconds =
                std::numeric_limits<time_t>::max();
            const std::uint64_t maximum_pts_seconds =
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()) / 1000000U;
            if (maximum_seconds > 0 &&
                static_cast<std::uint64_t>(maximum_seconds) >
                    maximum_pts_seconds) {
                value->tv_sec = maximum_seconds;
                value->tv_nsec = 999999999L;
            } else {
                value->tv_sec = 0;
                value->tv_nsec = 1000000000L;
            }
            return 0;
        }
        value->tv_sec = 9;
        value->tv_nsec = 7000L;
        return 0;
    }

    int last_error() const override { return last_error_; }

private:
    Mode mode_;
    int last_error_;
};

class RecordingObserver : public eavp::detail::V4L2Observer {
public:
    enum Callback {
        kNone,
        kCaptured,
        kWouldBlock,
        kPending,
        kDropped,
        kGap,
        kFatal
    };
    enum Failure { kStatusFailure, kBadAlloc, kUnexpected };

    RecordingObserver()
        : fail_callback(kNone), failure(kStatusFailure), captured(0U), bytes(0U),
          would_block(0U), pending_reports(), dropped(0U), gaps(0U), fatal(0U) {}

    eavp::Status on_captured(std::size_t copied_bytes) override {
        ++captured;
        bytes += copied_bytes;
        return result(kCaptured);
    }
    eavp::Status on_would_block() override {
        ++would_block;
        return result(kWouldBlock);
    }
    eavp::Status on_pending(bool value) override {
        pending_reports.push_back(value);
        return result(kPending);
    }
    eavp::Status on_dropped_on_stop() override {
        ++dropped;
        return result(kDropped);
    }
    eavp::Status on_sequence_gap(std::uint32_t missing_frames) override {
        gaps += missing_frames;
        return result(kGap);
    }
    eavp::Status on_fatal(const eavp::Status&) override {
        ++fatal;
        return result(kFatal);
    }

    Callback fail_callback;
    Failure failure;
    std::uint64_t captured;
    std::uint64_t bytes;
    std::uint64_t would_block;
    std::vector<bool> pending_reports;
    std::uint64_t dropped;
    std::uint64_t gaps;
    std::uint64_t fatal;

private:
    eavp::Status result(Callback callback) const {
        if (fail_callback != callback) return eavp::Status::ok_status();
        if (failure == kBadAlloc) throw std::bad_alloc();
        if (failure == kUnexpected) throw std::runtime_error("observer threw");
        return eavp::Status(eavp::StatusCode::kResourceExhausted,
                            "scripted observer failure");
    }
};

std::uint32_t fourcc(eavp::PixelFormat format) {
    if (format == eavp::PixelFormat::kNv12) return V4L2_PIX_FMT_NV12;
    if (format == eavp::PixelFormat::kYuyv422) return V4L2_PIX_FMT_YUYV;
    return V4L2_PIX_FMT_YUV420;
}

eavp::V4L2CaptureConfig make_config(eavp::PixelFormat format,
                                    int width, int height) {
    eavp::Result<eavp::V4L2CaptureConfig> result =
        eavp::V4L2CaptureConfig::create(
            "/dev/video-test", format, width, height, 30, 1, 3U);
    EXPECT_TRUE(result.ok());
    return result.take_value();
}

std::size_t count_after_first_dequeue(
    const std::vector<std::string>& streaming_calls,
    const std::string& operation_prefix) {
    bool dequeued = false;
    std::size_t count = 0U;
    for (std::size_t index = 0U; index < streaming_calls.size(); ++index) {
        if (streaming_calls[index] == "VIDIOC_DQBUF") {
            dequeued = true;
        } else if (dequeued &&
                   streaming_calls[index].find(operation_prefix) == 0U) {
            ++count;
        }
    }
    return count;
}

class V4L2NodeFixture {
public:
    V4L2NodeFixture(eavp::PixelFormat format_value,
                    int width_value, int height_value,
                    std::uint32_t stride_value, std::uint32_t capacity_value,
                    std::size_t sink_capacity = 4U,
                    eavp::OverflowPolicy policy = eavp::OverflowPolicy::kBlock,
                    ScriptedAllocator::Mode allocator_mode = ScriptedAllocator::kNormal,
                    RecordingObserver* observer_value = NULL,
                    eavp::MetricRegistry* metrics_value = NULL,
                    std::unique_ptr<eavp::detail::V4L2FormatFactory>
                        format_factory =
                            std::unique_ptr<eavp::detail::V4L2FormatFactory>(),
                    std::unique_ptr<eavp::detail::V4L2Clock> clock =
                        std::unique_ptr<eavp::detail::V4L2Clock>())
        : format(format_value), width(width_value), height(height_value),
          stride(stride_value), capacity(capacity_value),
          trace(new eavp_test::FakeV4L2Trace()),
          memory(new SourceMemory(3U, capacity_value)), api(NULL),
          metrics(metrics_value), sink("video_input", sink_capacity, policy), node() {
        std::unique_ptr<MemoryV4L2Api> fake(new MemoryV4L2Api(trace, memory));
        api = fake.get();
        api->set_format(fourcc(format), static_cast<std::uint32_t>(width),
                        static_cast<std::uint32_t>(height), stride, capacity);
        std::unique_ptr<eavp::detail::V4L2System> system(
            new eavp::detail::V4L2System(
                std::unique_ptr<eavp::detail::V4L2Api>(fake.release())));
        std::unique_ptr<eavp::detail::V4L2FrameAllocator> allocator(
            new ScriptedAllocator(allocator_mode));
        if (!clock) clock.reset(new ScriptedClock());
        eavp::Result<std::unique_ptr<eavp::V4L2SourceNode> > created =
            eavp::detail::V4L2SourceNodeTestPeer::create(
                "camera0", make_config(format, width, height), metrics,
                std::move(system), std::move(allocator), observer_value,
                std::move(format_factory), std::move(clock));
        EXPECT_TRUE(created.ok());
        node = created.take_value();
        EXPECT_TRUE(eavp::connect(node->output(), sink).ok());
    }

    eavp::Status start() {
        const eavp::Status prepared = node->prepare();
        return prepared.ok() ? node->start() : prepared;
    }

    void fill_visible(std::uint8_t first_value = 1U) {
        std::vector<std::uint8_t>& bytes = memory->regions[0U];
        std::fill(bytes.begin(), bytes.end(), 0xEEU);
        const std::vector<eavp::PlaneLayout> planes = expected_planes();
        const std::vector<std::size_t> visible = visible_row_bytes();
        std::uint8_t value = first_value;
        for (std::size_t plane = 0U; plane < planes.size(); ++plane) {
            const std::size_t rows = plane_rows(plane);
            for (std::size_t row = 0U; row < rows; ++row) {
                for (std::size_t column = 0U; column < visible[plane]; ++column) {
                    bytes[planes[plane].offset + row * planes[plane].stride + column] =
                        value++;
                }
            }
        }
    }

    std::vector<eavp::PlaneLayout> expected_planes() const {
        std::vector<eavp::PlaneLayout> planes;
        if (format == eavp::PixelFormat::kYuv420p) {
            const std::size_t y_size = static_cast<std::size_t>(stride) * height;
            const std::size_t c_stride = stride / 2U;
            const std::size_t c_size = c_stride * static_cast<std::size_t>(height / 2);
            planes.push_back(eavp::PlaneLayout(0U, y_size, stride));
            planes.push_back(eavp::PlaneLayout(y_size, c_size, c_stride));
            planes.push_back(eavp::PlaneLayout(y_size + c_size, c_size, c_stride));
        } else if (format == eavp::PixelFormat::kNv12) {
            const std::size_t y_size = static_cast<std::size_t>(stride) * height;
            const std::size_t uv_size = static_cast<std::size_t>(stride) * (height / 2);
            planes.push_back(eavp::PlaneLayout(0U, y_size, stride));
            planes.push_back(eavp::PlaneLayout(y_size, uv_size, stride));
        } else {
            planes.push_back(eavp::PlaneLayout(
                0U, static_cast<std::size_t>(stride) * height, stride));
        }
        return planes;
    }

    std::vector<std::size_t> visible_row_bytes() const {
        if (format == eavp::PixelFormat::kYuv420p) {
            return std::vector<std::size_t>{
                static_cast<std::size_t>(width),
                static_cast<std::size_t>(width / 2),
                static_cast<std::size_t>(width / 2)};
        }
        if (format == eavp::PixelFormat::kNv12) {
            return std::vector<std::size_t>{
                static_cast<std::size_t>(width),
                static_cast<std::size_t>(width)};
        }
        return std::vector<std::size_t>{static_cast<std::size_t>(width * 2)};
    }

    std::size_t plane_rows(std::size_t plane) const {
        if (format == eavp::PixelFormat::kYuyv422 || plane == 0U) {
            return static_cast<std::size_t>(height);
        }
        return static_cast<std::size_t>(height / 2);
    }

    std::size_t last_visible_end() const {
        const std::vector<eavp::PlaneLayout> planes = expected_planes();
        const std::vector<std::size_t> visible = visible_row_bytes();
        const std::size_t last = planes.size() - 1U;
        return planes[last].offset + planes[last].stride * (plane_rows(last) - 1U) +
               visible[last];
    }

    std::size_t visible_bytes() const {
        const std::vector<std::size_t> visible = visible_row_bytes();
        std::size_t result = 0U;
        for (std::size_t plane = 0U; plane < visible.size(); ++plane) {
            result += visible[plane] * plane_rows(plane);
        }
        return result;
    }

    void script_frame(std::uint32_t sequence = 0U,
                      long seconds = 2L, long microseconds = 3L,
                      std::uint32_t flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC,
                      std::uint32_t bytes_used = 0U,
                      std::uint32_t index = 0U) {
        if (bytes_used == 0U) {
            bytes_used = static_cast<std::uint32_t>(last_visible_end());
        }
        api->script_dequeued_buffer(index, bytes_used, flags, sequence,
                                    seconds, microseconds);
    }

    std::shared_ptr<const eavp::VideoFrame> take_frame() {
        return sink.receive().take_value();
    }

    std::size_t qbuf_after_dequeue_calls() const {
        return count_after_first_dequeue(trace->streaming_calls, "VIDIOC_QBUF:");
    }

    eavp::PixelFormat format;
    int width;
    int height;
    std::uint32_t stride;
    std::uint32_t capacity;
    std::shared_ptr<eavp_test::FakeV4L2Trace> trace;
    std::shared_ptr<SourceMemory> memory;
    MemoryV4L2Api* api;
    eavp::MetricRegistry* metrics;
    eavp::InputPort<eavp::VideoFrame> sink;
    std::unique_ptr<eavp::V4L2SourceNode> node;
};

void expect_copied_visible_and_zero_padding(
    const V4L2NodeFixture& fixture, const eavp::VideoFrame& frame,
    std::uint8_t first_value = 1U) {
    ASSERT_EQ(eavp::MemoryDomain::kCpu, frame.format().memory_domain());
    const std::vector<eavp::PlaneLayout> planes = fixture.expected_planes();
    const std::vector<std::size_t> visible = fixture.visible_row_bytes();
    std::uint8_t expected = first_value;
    for (std::size_t plane = 0U; plane < planes.size(); ++plane) {
        eavp::Result<eavp::MappedRegion> mapped =
            frame.buffer().map_plane(plane, eavp::MapMode::kReadOnly);
        ASSERT_TRUE(mapped.ok());
        eavp::MappedRegion region = mapped.take_value();
        for (std::size_t row = 0U; row < fixture.plane_rows(plane); ++row) {
            for (std::size_t column = 0U; column < visible[plane]; ++column) {
                EXPECT_EQ(expected++, region.data()[row * planes[plane].stride + column]);
            }
            for (std::size_t column = visible[plane];
                 column < planes[plane].stride; ++column) {
                EXPECT_EQ(0U, region.data()[row * planes[plane].stride + column]);
            }
        }
    }
}

static_assert(std::is_base_of<eavp::LinuxWaitSource,
                              eavp::V4L2SourceNode>::value,
              "V4L2 source must expose Linux readiness");

TEST(V4L2SourceNodeTest, CopiesPaddedYuv420VisibleRowsAndZeroesPadding) {
    V4L2NodeFixture fixture(eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U);
    fixture.fill_visible();
    fixture.script_frame();
    ASSERT_TRUE(fixture.start().ok());

    ASSERT_TRUE(fixture.node->tick().ok());
    ASSERT_EQ(1U, fixture.sink.queue_size());
    const std::shared_ptr<const eavp::VideoFrame> frame = fixture.take_frame();

    expect_copied_visible_and_zero_padding(fixture, *frame);
    EXPECT_EQ(1U, fixture.qbuf_after_dequeue_calls());
}

TEST(V4L2SourceNodeTest, CopiesPaddedNv12AndYuyvVisibleRows) {
    struct Case {
        eavp::PixelFormat format;
        std::uint32_t stride;
        std::uint32_t capacity;
    };
    const Case cases[] = {
        {eavp::PixelFormat::kNv12, 12U, 72U},
        {eavp::PixelFormat::kYuyv422, 20U, 80U},
    };
    for (std::size_t index = 0U; index < 2U; ++index) {
        V4L2NodeFixture fixture(
            cases[index].format, 8, 4, cases[index].stride, cases[index].capacity);
        fixture.fill_visible(static_cast<std::uint8_t>(20U + index));
        fixture.script_frame();
        ASSERT_TRUE(fixture.start().ok()) << index;
        ASSERT_TRUE(fixture.node->tick().ok()) << index;
        const std::shared_ptr<const eavp::VideoFrame> frame = fixture.take_frame();
        expect_copied_visible_and_zero_padding(
            fixture, *frame, static_cast<std::uint8_t>(20U + index));
        EXPECT_EQ(1U, fixture.qbuf_after_dequeue_calls()) << index;
    }
}

TEST(V4L2SourceNodeTest, RejectsMissingFinalVisibleByteAndStillRequeues) {
    V4L2NodeFixture fixture(eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U);
    fixture.fill_visible();
    fixture.script_frame(0U, 2L, 3L, V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC,
                         static_cast<std::uint32_t>(fixture.last_visible_end() - 1U));
    ASSERT_TRUE(fixture.start().ok());

    const eavp::Status status = fixture.node->tick();

    EXPECT_EQ(eavp::StatusCode::kCorruptData, status.code());
    EXPECT_EQ(1U, fixture.qbuf_after_dequeue_calls());
    EXPECT_EQ(0U, fixture.sink.queue_size());
}

TEST(V4L2SourceNodeTest, AllocationMapAndFrameFailuresStillRequeue) {
    struct Case {
        ScriptedAllocator::Mode mode;
        eavp::StatusCode expected;
    };
    const Case cases[] = {
        {ScriptedAllocator::kAllocationFailure,
         eavp::StatusCode::kResourceExhausted},
        {ScriptedAllocator::kMapFailure, eavp::StatusCode::kIoError},
        {ScriptedAllocator::kWrongDomain,
         eavp::StatusCode::kCapabilityMismatch},
    };
    for (std::size_t index = 0U; index < 3U; ++index) {
        V4L2NodeFixture fixture(
            eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U, 4U,
            eavp::OverflowPolicy::kBlock, cases[index].mode);
        fixture.fill_visible();
        fixture.script_frame();
        ASSERT_TRUE(fixture.start().ok()) << index;

        const eavp::Status status = fixture.node->tick();

        EXPECT_EQ(cases[index].expected, status.code()) << index;
        EXPECT_EQ(1U, fixture.qbuf_after_dequeue_calls()) << index;
    }
}

TEST(V4L2SourceNodeTest, AllocatorExceptionsAfterDequeueAreContainedAndRequeued) {
    const ScriptedAllocator::Mode modes[] = {
        ScriptedAllocator::kBadAlloc, ScriptedAllocator::kUnexpected};
    const eavp::StatusCode expected[] = {
        eavp::StatusCode::kResourceExhausted, eavp::StatusCode::kInternal};
    for (std::size_t index = 0U; index < 2U; ++index) {
        V4L2NodeFixture fixture(
            eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U, 4U,
            eavp::OverflowPolicy::kBlock, modes[index]);
        fixture.fill_visible();
        fixture.script_frame();
        ASSERT_TRUE(fixture.start().ok()) << index;

        const eavp::Status status = fixture.node->tick();

        EXPECT_EQ(expected[index], status.code()) << index;
        EXPECT_TRUE(status.message().empty()) << index;
        EXPECT_EQ(1U, fixture.qbuf_after_dequeue_calls()) << index;
    }
}

TEST(V4L2SourceNodeTest, DriverErrorFlagStillRequeues) {
    V4L2NodeFixture fixture(eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U);
    fixture.fill_visible();
    fixture.script_frame(0U, 2L, 3L,
                         V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC | V4L2_BUF_FLAG_ERROR);
    ASSERT_TRUE(fixture.start().ok());

    const eavp::Status status = fixture.node->tick();

    EXPECT_EQ(eavp::StatusCode::kCorruptData, status.code());
    EXPECT_EQ(1U, fixture.qbuf_after_dequeue_calls());
}

TEST(V4L2SourceNodeTest, RequeueFailureWinsOverMediaFailureExactly) {
    V4L2NodeFixture fixture(
        eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U, 4U,
        eavp::OverflowPolicy::kBlock, ScriptedAllocator::kAllocationFailure);
    fixture.fill_visible();
    fixture.script_frame();
    fixture.api->script_error("VIDIOC_QBUF:0", 0);
    fixture.api->script_error("VIDIOC_QBUF:0", EIO);
    ASSERT_TRUE(fixture.start().ok());

    const eavp::Status status = fixture.node->tick();

    EXPECT_EQ(eavp::StatusCode::kIoError, status.code());
    EXPECT_EQ("v4l2", status.provider_id());
    EXPECT_EQ("VIDIOC_QBUF", status.operation());
    ASSERT_TRUE(status.has_native_code());
    EXPECT_EQ(EIO, status.native_code());
    EXPECT_EQ(1U, fixture.qbuf_after_dequeue_calls());
}

TEST(V4L2SourceNodeTest, RequeueFailureReportsFatalWithoutBeingOverwritten) {
    RecordingObserver observer;
    observer.fail_callback = RecordingObserver::kFatal;
    V4L2NodeFixture fixture(
        eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U, 4U,
        eavp::OverflowPolicy::kBlock,
        ScriptedAllocator::kAllocationFailure, &observer);
    fixture.fill_visible();
    fixture.script_frame();
    fixture.api->script_error("VIDIOC_QBUF:0", 0);
    fixture.api->script_error("VIDIOC_QBUF:0", EIO);
    ASSERT_TRUE(fixture.start().ok());

    const eavp::Status status = fixture.node->tick();

    EXPECT_EQ(eavp::StatusCode::kIoError, status.code());
    EXPECT_EQ("v4l2", status.provider_id());
    EXPECT_EQ("VIDIOC_QBUF", status.operation());
    ASSERT_TRUE(status.has_native_code());
    EXPECT_EQ(EIO, status.native_code());
    EXPECT_EQ(1U, observer.fatal);
    EXPECT_EQ(1U, fixture.qbuf_after_dequeue_calls());
}

TEST(V4L2SourceNodeTest, FrameOwnsCpuCopyAfterDriverMemoryChanges) {
    V4L2NodeFixture fixture(eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U);
    fixture.fill_visible(41U);
    fixture.script_frame();
    ASSERT_TRUE(fixture.start().ok());
    ASSERT_TRUE(fixture.node->tick().ok());
    const std::shared_ptr<const eavp::VideoFrame> frame = fixture.take_frame();

    std::fill(fixture.memory->regions[0U].begin(),
              fixture.memory->regions[0U].end(), 0xCCU);

    expect_copied_visible_and_zero_padding(fixture, *frame, 41U);
}

std::size_t operation_count(const std::vector<std::string>& operations,
                            const std::string& expected) {
    return static_cast<std::size_t>(
        std::count(operations.begin(), operations.end(), expected));
}

std::uint64_t counter_or_zero(const eavp::MetricRegistry& metrics,
                              const std::string& name) {
    const eavp::Result<std::uint64_t> result = metrics.counter(name);
    return result.ok() ? result.value() : 0U;
}

double gauge_or_negative(const eavp::MetricRegistry& metrics,
                         const std::string& name) {
    const eavp::Result<double> result = metrics.gauge(name);
    return result.ok() ? result.value() : -1.0;
}

std::int64_t monotonic_us() {
    struct timespec now;
    std::memset(&now, 0, sizeof(now));
    EXPECT_EQ(0, ::clock_gettime(CLOCK_MONOTONIC, &now));
    return static_cast<std::int64_t>(now.tv_sec) * 1000000 +
           static_cast<std::int64_t>(now.tv_nsec / 1000);
}

TEST(V4L2SourceNodeTest, FactoryRejectsEmptyIdAndAllowsNullMetrics) {
    const eavp::V4L2CaptureConfig config =
        make_config(eavp::PixelFormat::kYuv420p, 8, 4);

    const eavp::Result<std::unique_ptr<eavp::V4L2SourceNode> > empty =
        eavp::V4L2SourceNode::create("", config, NULL);
    eavp::Result<std::unique_ptr<eavp::V4L2SourceNode> > without_metrics =
        eavp::V4L2SourceNode::create("camera0", config, NULL);

    ASSERT_FALSE(empty.ok());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument, empty.status().code());
    EXPECT_TRUE(without_metrics.ok());
}

TEST(V4L2SourceNodeTest, ActualFormatExistsOnlyBetweenPrepareAndReset) {
    V4L2NodeFixture fixture(eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U);

    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              fixture.node->actual_format().status().code());
    ASSERT_TRUE(fixture.node->prepare().ok());
    const eavp::Result<eavp::VideoFormat> actual =
        fixture.node->actual_format();
    ASSERT_TRUE(actual.ok());
    EXPECT_EQ(eavp::PixelFormat::kYuv420p, actual.value().pixel_format());
    EXPECT_EQ(8, actual.value().width());
    EXPECT_EQ(4, actual.value().height());
    EXPECT_EQ(eavp::MemoryDomain::kMmap, actual.value().memory_domain());
    ASSERT_EQ(3U, actual.value().planes().size());
    const std::vector<eavp::PlaneLayout> expected = fixture.expected_planes();
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        EXPECT_EQ(expected[index].offset, actual.value().planes()[index].offset);
        EXPECT_EQ(expected[index].size, actual.value().planes()[index].size);
        EXPECT_EQ(expected[index].stride, actual.value().planes()[index].stride);
    }
    ASSERT_TRUE(fixture.node->reset().ok());
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              fixture.node->actual_format().status().code());
}

TEST(V4L2SourceNodeTest, PreparePostFailureRollsBackAndCanRetry) {
    const FailOnceFormatFactory::Failure failures[] = {
        FailOnceFormatFactory::kStatusFailure,
        FailOnceFormatFactory::kBadAlloc,
        FailOnceFormatFactory::kUnexpected};
    const eavp::StatusCode expected[] = {
        eavp::StatusCode::kResourceExhausted,
        eavp::StatusCode::kResourceExhausted,
        eavp::StatusCode::kInternal};
    for (std::size_t index = 0U; index < 3U; ++index) {
        std::unique_ptr<eavp::detail::V4L2FormatFactory> format_factory(
            new FailOnceFormatFactory(failures[index]));
        V4L2NodeFixture fixture(
            eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U, 4U,
            eavp::OverflowPolicy::kBlock, ScriptedAllocator::kNormal,
            NULL, NULL, std::move(format_factory));
        if (index == 0U) {
            fixture.api->script_error("VIDIOC_REQBUFS(0)", EIO);
        }

        const eavp::Status first = fixture.node->prepare();

        EXPECT_EQ(expected[index], first.code()) << index;
        if (index == 0U) {
            EXPECT_EQ("scripted format failure", first.message());
        } else {
            EXPECT_TRUE(first.message().empty()) << index;
        }
        EXPECT_EQ(eavp::StatusCode::kInvalidState,
                  fixture.node->actual_format().status().code()) << index;
        EXPECT_EQ(1U, fixture.trace->close_calls) << index;
        EXPECT_TRUE(fixture.node->reset().ok()) << index;
        EXPECT_TRUE(fixture.node->prepare().ok()) << index;
        EXPECT_TRUE(fixture.node->actual_format().ok()) << index;
        EXPECT_EQ(2U, fixture.trace->open_calls) << index;
    }
}

TEST(V4L2SourceNodeTest, WaitSourceDelegatesOnlyWhileRunning) {
    V4L2NodeFixture fixture(eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U);

    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              fixture.node->poll_descriptors().status().code());
    ASSERT_TRUE(fixture.node->prepare().ok());
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              fixture.node->poll_descriptors().status().code());
    ASSERT_TRUE(fixture.node->start().ok());
    eavp::Result<std::vector<struct pollfd> > descriptors =
        fixture.node->poll_descriptors();
    ASSERT_TRUE(descriptors.ok());
    ASSERT_EQ(1U, descriptors.value().size());
    EXPECT_EQ(41, descriptors.value()[0].fd);
    descriptors.value()[0].revents = POLLIN;
    const eavp::Result<bool> ready =
        fixture.node->evaluate_poll_events(descriptors.value());
    ASSERT_TRUE(ready.ok());
    EXPECT_TRUE(ready.value());
    ASSERT_TRUE(fixture.node->stop().ok());
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              fixture.node->poll_descriptors().status().code());
}

TEST(V4L2SourceNodeTest, RestartsPreparedSessionAndClearsPtsAndSequence) {
    eavp::MetricRegistry metrics;
    V4L2NodeFixture fixture(
        eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U, 4U,
        eavp::OverflowPolicy::kBlock, ScriptedAllocator::kNormal,
        NULL, &metrics);
    fixture.fill_visible();
    fixture.script_frame(100U, 50L, 0L);
    ASSERT_TRUE(fixture.start().ok());
    ASSERT_TRUE(fixture.node->tick().ok());
    EXPECT_EQ(50000000, fixture.take_frame()->pts());
    ASSERT_TRUE(fixture.node->stop().ok());

    fixture.script_frame(1U, 2L, 0L);
    ASSERT_TRUE(fixture.node->prepare().ok());
    ASSERT_TRUE(fixture.node->start().ok());
    ASSERT_TRUE(fixture.node->tick().ok());

    EXPECT_EQ(2000000, fixture.take_frame()->pts());
    EXPECT_EQ(0U, counter_or_zero(metrics, "v4l2.sequence.gaps"));
    EXPECT_EQ(1U, fixture.trace->open_calls);
}

TEST(V4L2SourceNodeTest, StartObserverFailureRollsBackStreamingAndCanRetry) {
    RecordingObserver observer;
    observer.fail_callback = RecordingObserver::kPending;
    V4L2NodeFixture fixture(
        eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U, 4U,
        eavp::OverflowPolicy::kBlock, ScriptedAllocator::kNormal,
        &observer);
    ASSERT_TRUE(fixture.node->prepare().ok());

    const eavp::Status first = fixture.node->start();

    EXPECT_EQ(eavp::StatusCode::kResourceExhausted, first.code());
    EXPECT_EQ(1U, fixture.trace->stream_on_calls);
    EXPECT_EQ(1U, fixture.trace->stream_off_calls);
    observer.fail_callback = RecordingObserver::kNone;
    EXPECT_TRUE(fixture.node->stop().ok());
    EXPECT_TRUE(fixture.node->prepare().ok());
    EXPECT_TRUE(fixture.node->start().ok());
    EXPECT_EQ(2U, fixture.trace->stream_on_calls);
    EXPECT_EQ(1U, fixture.trace->open_calls);
}

TEST(V4L2SourceNodeTest, StartRollbackFailureWinsAndReportsFatal) {
    RecordingObserver observer;
    observer.fail_callback = RecordingObserver::kPending;
    V4L2NodeFixture fixture(
        eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U, 4U,
        eavp::OverflowPolicy::kBlock, ScriptedAllocator::kNormal,
        &observer);
    ASSERT_TRUE(fixture.node->prepare().ok());
    fixture.api->script_error("VIDIOC_STREAMOFF", EIO);

    const eavp::Status status = fixture.node->start();

    EXPECT_EQ(eavp::StatusCode::kIoError, status.code());
    EXPECT_EQ("v4l2", status.provider_id());
    EXPECT_EQ("VIDIOC_STREAMOFF", status.operation());
    ASSERT_TRUE(status.has_native_code());
    EXPECT_EQ(EIO, status.native_code());
    EXPECT_EQ(1U, observer.fatal);
    EXPECT_TRUE(fixture.node->reset().ok());
    EXPECT_EQ(2U, fixture.trace->stream_off_calls);
}

TEST(V4L2SourceNodeTest, PendingFrameStopsDequeueUntilDeliverySucceeds) {
    RecordingObserver observer;
    V4L2NodeFixture fixture(
        eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U, 1U,
        eavp::OverflowPolicy::kBlock, ScriptedAllocator::kNormal, &observer);
    fixture.fill_visible();
    fixture.script_frame(0U);
    fixture.script_frame(1U);
    ASSERT_TRUE(fixture.start().ok());
    ASSERT_TRUE(fixture.node->tick().ok());
    ASSERT_EQ(eavp::StatusCode::kWouldBlock, fixture.node->tick().code());
    const std::size_t dequeues_after_backpressure =
        operation_count(fixture.trace->streaming_calls, "VIDIOC_DQBUF");

    EXPECT_EQ(eavp::StatusCode::kWouldBlock, fixture.node->tick().code());
    EXPECT_EQ(dequeues_after_backpressure,
              operation_count(fixture.trace->streaming_calls, "VIDIOC_DQBUF"));
    fixture.take_frame();
    EXPECT_TRUE(fixture.node->tick().ok());
    EXPECT_EQ(dequeues_after_backpressure,
              operation_count(fixture.trace->streaming_calls, "VIDIOC_DQBUF"));
    EXPECT_EQ(1U, fixture.sink.queue_size());
    ASSERT_GE(observer.pending_reports.size(), 3U);
    EXPECT_FALSE(observer.pending_reports[0U]);
    EXPECT_TRUE(observer.pending_reports[1U]);
    EXPECT_FALSE(observer.pending_reports.back());
}

TEST(V4L2SourceNodeTest, OutputBackpressureDoesNotPublishDequeueWouldBlock) {
    eavp::MetricRegistry metrics;
    V4L2NodeFixture fixture(
        eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U, 1U,
        eavp::OverflowPolicy::kBlock, ScriptedAllocator::kNormal,
        NULL, &metrics);
    fixture.fill_visible();
    fixture.script_frame(0U);
    fixture.script_frame(1U);
    ASSERT_TRUE(fixture.start().ok());
    ASSERT_TRUE(fixture.node->tick().ok());
    ASSERT_EQ(eavp::StatusCode::kWouldBlock, fixture.node->tick().code());
    const std::size_t dequeues_with_pending =
        operation_count(fixture.trace->streaming_calls, "VIDIOC_DQBUF");

    EXPECT_EQ(0U, counter_or_zero(metrics, "v4l2.dequeue.would_block"));
    EXPECT_EQ(eavp::StatusCode::kWouldBlock, fixture.node->tick().code());
    EXPECT_EQ(dequeues_with_pending,
              operation_count(fixture.trace->streaming_calls, "VIDIOC_DQBUF"));
    EXPECT_EQ(0U, counter_or_zero(metrics, "v4l2.dequeue.would_block"));
    fixture.take_frame();
    EXPECT_TRUE(fixture.node->tick().ok());
    EXPECT_EQ(dequeues_with_pending,
              operation_count(fixture.trace->streaming_calls, "VIDIOC_DQBUF"));
    EXPECT_EQ(eavp::StatusCode::kWouldBlock, fixture.node->tick().code());
    EXPECT_EQ(1U, counter_or_zero(metrics, "v4l2.dequeue.would_block"));
}

TEST(V4L2SourceNodeTest, StopDropsPendingAndPublishesDropAndGauge) {
    eavp::MetricRegistry metrics;
    V4L2NodeFixture fixture(
        eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U, 1U,
        eavp::OverflowPolicy::kBlock, ScriptedAllocator::kNormal, NULL, &metrics);
    fixture.fill_visible();
    fixture.script_frame(0U);
    fixture.script_frame(1U);
    ASSERT_TRUE(fixture.start().ok());
    ASSERT_TRUE(fixture.node->tick().ok());
    ASSERT_EQ(eavp::StatusCode::kWouldBlock, fixture.node->tick().code());
    ASSERT_EQ(1.0, gauge_or_negative(metrics, "v4l2.pending_frame"));

    EXPECT_TRUE(fixture.node->stop().ok());

    EXPECT_EQ(1U, counter_or_zero(metrics, "v4l2.frames.dropped_on_stop"));
    EXPECT_EQ(0.0, gauge_or_negative(metrics, "v4l2.pending_frame"));
    EXPECT_EQ(1U, fixture.trace->stream_off_calls);
}

TEST(V4L2SourceNodeTest, DropNewestQueueDoesNotCreatePendingFrame) {
    eavp::MetricRegistry metrics;
    V4L2NodeFixture fixture(
        eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U, 1U,
        eavp::OverflowPolicy::kDropNewest, ScriptedAllocator::kNormal,
        NULL, &metrics);
    fixture.fill_visible();
    fixture.script_frame(0U);
    fixture.script_frame(1U);
    ASSERT_TRUE(fixture.start().ok());

    EXPECT_TRUE(fixture.node->tick().ok());
    EXPECT_TRUE(fixture.node->tick().ok());

    EXPECT_EQ(1U, fixture.sink.queue_size());
    EXPECT_EQ(1U, fixture.sink.dropped_count());
    EXPECT_EQ(0.0, gauge_or_negative(metrics, "v4l2.pending_frame"));
    EXPECT_EQ(2U, counter_or_zero(metrics, "v4l2.frames.captured"));
}

TEST(V4L2SourceNodeTest, UsesValidMonotonicDriverTimestampInMicroseconds) {
    V4L2NodeFixture fixture(eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U);
    fixture.fill_visible();
    fixture.script_frame(0U, 7L, 123L);
    ASSERT_TRUE(fixture.start().ok());
    ASSERT_TRUE(fixture.node->tick().ok());
    const std::shared_ptr<const eavp::VideoFrame> frame = fixture.take_frame();

    EXPECT_EQ(7000123, frame->pts());
    EXPECT_EQ(1, frame->time_base().numerator());
    EXPECT_EQ(1000000, frame->time_base().denominator());
}

TEST(V4L2SourceNodeTest, InvalidOrOverflowingDriverTimestampsUseMonotonicFallback) {
    struct Case {
        std::uint32_t flags;
        long seconds;
        long microseconds;
    };
    const Case cases[] = {
        {V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC, 0L, 0L},
        {0U, 5L, 2L},
        {V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC, -1L, 2L},
        {V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC, 1L, -1L},
        {V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC, 1L, 1000000L},
    };
    for (std::size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        V4L2NodeFixture fixture(eavp::PixelFormat::kYuv420p,
                                8, 4, 12U, 80U);
        fixture.fill_visible();
        fixture.script_frame(0U, cases[index].seconds,
                             cases[index].microseconds, cases[index].flags);
        ASSERT_TRUE(fixture.start().ok()) << index;
        ASSERT_TRUE(fixture.node->tick().ok()) << index;
        const std::shared_ptr<const eavp::VideoFrame> frame = fixture.take_frame();
        EXPECT_EQ(9000007, frame->pts()) << index;
    }

    V4L2NodeFixture boundary(eavp::PixelFormat::kYuv420p,
                             8, 4, 12U, 80U);
    boundary.fill_visible();
    const std::uint64_t first_overflowing_second =
        static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()) / 1000000U + 1U;
    const bool long_can_express_overflow =
        static_cast<std::uint64_t>(LONG_MAX) >= first_overflowing_second;
    const long seconds = long_can_express_overflow
        ? static_cast<long>(first_overflowing_second) : LONG_MAX;
    boundary.script_frame(0U, seconds, 999999L);
    ASSERT_TRUE(boundary.start().ok());
    ASSERT_TRUE(boundary.node->tick().ok());
    const std::shared_ptr<const eavp::VideoFrame> frame =
        boundary.take_frame();
    if (long_can_express_overflow) {
        EXPECT_EQ(9000007, frame->pts());
    } else {
        const std::int64_t maximum_legal =
            timestamp_microseconds(seconds, 999999L);
        EXPECT_EQ(maximum_legal, frame->pts());
    }
}

TEST(V4L2SourceNodeTest, FallbackClockFailuresRequeueWithoutOutput) {
    const ScriptedClock::Mode modes[] = {
        ScriptedClock::kSyscallFailure,
        ScriptedClock::kInvalidTimespec,
        ScriptedClock::kBadAlloc,
        ScriptedClock::kUnexpected};
    const eavp::StatusCode expected[] = {
        eavp::StatusCode::kIoError,
        eavp::StatusCode::kCorruptData,
        eavp::StatusCode::kResourceExhausted,
        eavp::StatusCode::kInternal};
    for (std::size_t index = 0U; index < 4U; ++index) {
        std::unique_ptr<eavp::detail::V4L2Clock> clock(
            new ScriptedClock(modes[index]));
        V4L2NodeFixture fixture(
            eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U, 4U,
            eavp::OverflowPolicy::kBlock, ScriptedAllocator::kNormal,
            NULL, NULL,
            std::unique_ptr<eavp::detail::V4L2FormatFactory>(),
            std::move(clock));
        fixture.fill_visible();
        fixture.script_frame(0U, 0L, 0L);
        ASSERT_TRUE(fixture.start().ok()) << index;

        const eavp::Status status = fixture.node->tick();

        EXPECT_EQ(expected[index], status.code()) << index;
        if (modes[index] == ScriptedClock::kSyscallFailure) {
            EXPECT_EQ("v4l2", status.provider_id());
            EXPECT_EQ("clock_gettime(CLOCK_MONOTONIC)", status.operation());
            ASSERT_TRUE(status.has_native_code());
            EXPECT_EQ(EIO, status.native_code());
        }
        if (modes[index] == ScriptedClock::kInvalidTimespec) {
            EXPECT_EQ("V4L2 monotonic timestamp is invalid or overflows",
                      status.message());
            EXPECT_TRUE(status.provider_id().empty());
            EXPECT_TRUE(status.operation().empty());
            EXPECT_FALSE(status.has_native_code());
        }
        if (modes[index] == ScriptedClock::kBadAlloc ||
            modes[index] == ScriptedClock::kUnexpected) {
            EXPECT_TRUE(status.message().empty()) << index;
        }
        EXPECT_EQ(1U, fixture.qbuf_after_dequeue_calls()) << index;
        EXPECT_EQ(0U, fixture.sink.queue_size()) << index;
    }
}

TEST(V4L2SourceNodeTest, ClampsFallbackPtsToPreviousDriverPts) {
    const std::int64_t future_seconds = monotonic_us() / 1000000 + 100;
    V4L2NodeFixture fixture(eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U);
    fixture.fill_visible();
    fixture.script_frame(0U, static_cast<long>(future_seconds), 9L);
    fixture.script_frame(1U, 0L, 0L);
    ASSERT_TRUE(fixture.start().ok());
    ASSERT_TRUE(fixture.node->tick().ok());
    ASSERT_TRUE(fixture.node->tick().ok());
    const std::int64_t first_pts = fixture.take_frame()->pts();
    const std::int64_t second_pts = fixture.take_frame()->pts();

    EXPECT_EQ(future_seconds * 1000000 + 9, first_pts);
    EXPECT_EQ(first_pts, second_pts);
}

TEST(V4L2SourceNodeTest, CountsForwardSequenceGapsAndAcceptsUint32Wrap) {
    eavp::MetricRegistry gap_metrics;
    V4L2NodeFixture gaps(
        eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U, 4U,
        eavp::OverflowPolicy::kBlock, ScriptedAllocator::kNormal,
        NULL, &gap_metrics);
    gaps.fill_visible();
    gaps.script_frame(10U);
    gaps.script_frame(13U);
    ASSERT_TRUE(gaps.start().ok());
    ASSERT_TRUE(gaps.node->tick().ok());
    ASSERT_TRUE(gaps.node->tick().ok());
    EXPECT_EQ(2U, counter_or_zero(gap_metrics, "v4l2.sequence.gaps"));

    eavp::MetricRegistry wrap_metrics;
    V4L2NodeFixture wrap(
        eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U, 4U,
        eavp::OverflowPolicy::kBlock, ScriptedAllocator::kNormal,
        NULL, &wrap_metrics);
    wrap.fill_visible();
    wrap.script_frame(UINT32_MAX);
    wrap.script_frame(0U);
    ASSERT_TRUE(wrap.start().ok());
    ASSERT_TRUE(wrap.node->tick().ok());
    ASSERT_TRUE(wrap.node->tick().ok());
    EXPECT_EQ(0U, counter_or_zero(wrap_metrics, "v4l2.sequence.gaps"));
}

TEST(V4L2SourceNodeTest, RejectsClearlyReverseSequenceAndStillRequeues) {
    V4L2NodeFixture fixture(eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U);
    fixture.fill_visible();
    fixture.script_frame(10U);
    fixture.script_frame(9U);
    ASSERT_TRUE(fixture.start().ok());
    ASSERT_TRUE(fixture.node->tick().ok());

    const eavp::Status status = fixture.node->tick();

    EXPECT_EQ(eavp::StatusCode::kCorruptData, status.code());
    EXPECT_EQ(2U, fixture.qbuf_after_dequeue_calls());
}

TEST(V4L2SourceNodeTest, RejectsSequenceDeltaAtHalfRangeAndStillRequeues) {
    V4L2NodeFixture fixture(eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U);
    fixture.fill_visible();
    fixture.script_frame(0U);
    fixture.script_frame(0x80000001U);
    ASSERT_TRUE(fixture.start().ok());
    ASSERT_TRUE(fixture.node->tick().ok());

    const eavp::Status status = fixture.node->tick();

    EXPECT_EQ(eavp::StatusCode::kCorruptData, status.code());
    EXPECT_EQ(2U, fixture.qbuf_after_dequeue_calls());
    EXPECT_EQ(1U, fixture.sink.queue_size());
}

TEST(V4L2SourceNodeTest, PublishesCapturedBytesWouldBlockGapAndPendingMetrics) {
    eavp::MetricRegistry metrics;
    V4L2NodeFixture fixture(
        eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U, 4U,
        eavp::OverflowPolicy::kBlock, ScriptedAllocator::kNormal,
        NULL, &metrics);
    fixture.fill_visible();
    fixture.script_frame(40U);
    fixture.script_frame(43U);
    ASSERT_TRUE(fixture.start().ok());
    ASSERT_TRUE(fixture.node->tick().ok());
    ASSERT_TRUE(fixture.node->tick().ok());
    EXPECT_EQ(eavp::StatusCode::kWouldBlock, fixture.node->tick().code());

    EXPECT_EQ(2U, counter_or_zero(metrics, "v4l2.frames.captured"));
    EXPECT_EQ(2U * fixture.visible_bytes(),
              counter_or_zero(metrics, "v4l2.bytes.copied"));
    EXPECT_EQ(1U, counter_or_zero(metrics, "v4l2.dequeue.would_block"));
    EXPECT_EQ(2U, counter_or_zero(metrics, "v4l2.sequence.gaps"));
    EXPECT_EQ(0.0, gauge_or_negative(metrics, "v4l2.pending_frame"));
}

TEST(V4L2SourceNodeTest, MediaFailureWinsOverFatalObserverFailure) {
    RecordingObserver observer;
    observer.fail_callback = RecordingObserver::kFatal;
    V4L2NodeFixture fixture(
        eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U, 4U,
        eavp::OverflowPolicy::kBlock,
        ScriptedAllocator::kAllocationFailure, &observer);
    fixture.fill_visible();
    fixture.script_frame();
    ASSERT_TRUE(fixture.start().ok());

    const eavp::Status status = fixture.node->tick();

    EXPECT_EQ(eavp::StatusCode::kResourceExhausted, status.code());
    EXPECT_EQ("scripted allocation failure", status.message());
    EXPECT_EQ(1U, observer.fatal);
    EXPECT_EQ(1U, fixture.qbuf_after_dequeue_calls());
}

TEST(V4L2SourceNodeTest, StandaloneObserverFailuresPropagateAfterRequiredEffects) {
    {
        RecordingObserver observer;
        observer.fail_callback = RecordingObserver::kCaptured;
        V4L2NodeFixture fixture(
            eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U, 4U,
            eavp::OverflowPolicy::kBlock, ScriptedAllocator::kNormal, &observer);
        fixture.fill_visible();
        fixture.script_frame();
        ASSERT_TRUE(fixture.start().ok());
        const eavp::Status status = fixture.node->tick();
        EXPECT_EQ(eavp::StatusCode::kResourceExhausted, status.code());
        EXPECT_EQ(1U, fixture.qbuf_after_dequeue_calls());
    }
    {
        RecordingObserver observer;
        observer.fail_callback = RecordingObserver::kWouldBlock;
        V4L2NodeFixture fixture(
            eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U, 4U,
            eavp::OverflowPolicy::kBlock, ScriptedAllocator::kNormal, &observer);
        ASSERT_TRUE(fixture.start().ok());
        const eavp::Status status = fixture.node->tick();
        EXPECT_EQ(eavp::StatusCode::kResourceExhausted, status.code());
        EXPECT_EQ(1U, observer.would_block);
    }
    {
        RecordingObserver observer;
        V4L2NodeFixture fixture(
            eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U, 1U,
            eavp::OverflowPolicy::kBlock, ScriptedAllocator::kNormal, &observer);
        fixture.fill_visible();
        fixture.script_frame(0U);
        fixture.script_frame(1U);
        ASSERT_TRUE(fixture.start().ok());
        ASSERT_TRUE(fixture.node->tick().ok());
        observer.fail_callback = RecordingObserver::kPending;
        const eavp::Status status = fixture.node->tick();
        EXPECT_EQ(eavp::StatusCode::kResourceExhausted, status.code());
        EXPECT_EQ(2U, fixture.qbuf_after_dequeue_calls());
    }
    {
        RecordingObserver observer;
        V4L2NodeFixture fixture(
            eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U, 4U,
            eavp::OverflowPolicy::kBlock, ScriptedAllocator::kNormal, &observer);
        fixture.fill_visible();
        fixture.script_frame(10U);
        fixture.script_frame(13U);
        ASSERT_TRUE(fixture.start().ok());
        ASSERT_TRUE(fixture.node->tick().ok());
        observer.fail_callback = RecordingObserver::kGap;
        const eavp::Status status = fixture.node->tick();
        EXPECT_EQ(eavp::StatusCode::kResourceExhausted, status.code());
        EXPECT_EQ(2U, observer.gaps);
        EXPECT_EQ(2U, fixture.qbuf_after_dequeue_calls());
    }
}

TEST(V4L2SourceNodeTest, StopStillStreamsOffWhenDropObserverFails) {
    RecordingObserver observer;
    V4L2NodeFixture fixture(
        eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U, 1U,
        eavp::OverflowPolicy::kBlock, ScriptedAllocator::kNormal, &observer);
    fixture.fill_visible();
    fixture.script_frame(0U);
    fixture.script_frame(1U);
    ASSERT_TRUE(fixture.start().ok());
    ASSERT_TRUE(fixture.node->tick().ok());
    ASSERT_EQ(eavp::StatusCode::kWouldBlock, fixture.node->tick().code());
    observer.fail_callback = RecordingObserver::kDropped;

    const eavp::Status status = fixture.node->stop();

    EXPECT_EQ(eavp::StatusCode::kResourceExhausted, status.code());
    EXPECT_EQ(1U, observer.dropped);
    EXPECT_EQ(1U, fixture.trace->stream_off_calls);
}

TEST(V4L2SourceNodeTest, ObserverExceptionsNeverCrossNodeBoundary) {
    const RecordingObserver::Failure failures[] = {
        RecordingObserver::kBadAlloc, RecordingObserver::kUnexpected};
    const eavp::StatusCode expected[] = {
        eavp::StatusCode::kResourceExhausted, eavp::StatusCode::kInternal};
    for (std::size_t index = 0U; index < 2U; ++index) {
        RecordingObserver observer;
        observer.fail_callback = RecordingObserver::kWouldBlock;
        observer.failure = failures[index];
        V4L2NodeFixture fixture(
            eavp::PixelFormat::kYuv420p, 8, 4, 12U, 80U, 4U,
            eavp::OverflowPolicy::kBlock, ScriptedAllocator::kNormal, &observer);
        ASSERT_TRUE(fixture.start().ok()) << index;

        const eavp::Status status = fixture.node->tick();

        EXPECT_EQ(expected[index], status.code()) << index;
        EXPECT_TRUE(status.message().empty()) << index;
    }
}

}  // namespace
