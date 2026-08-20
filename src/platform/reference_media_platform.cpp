#include "eavp/platform/reference_media_platform.hpp"

#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "eavp/media/backend_node.hpp"
#include "eavp/media/frame.hpp"
#include "eavp/media/port.hpp"

namespace eavp {

namespace {

const char kPipelineId[] = "reference0";

Status allocation_failure() {
    return Status(StatusCode::kResourceExhausted,
                  "reference media platform allocation failed");
}

Status unexpected_failure() {
    return Status(StatusCode::kInternal,
                  "reference media platform caught an unexpected exception");
}

class ReferenceCpuStorage : public BufferStorage {
public:
    explicit ReferenceCpuStorage(std::size_t size)
        : bytes_(size), provider_id_("reference.source") {}

    MemoryDomain memory_domain() const { return MemoryDomain::kCpu; }
    std::size_t capacity() const { return bytes_.size(); }
    const std::string& provider_id() const { return provider_id_; }

    Status map(MapMode, std::uint8_t** data, std::size_t* size) {
        if (data == NULL || size == NULL) {
            return Status(StatusCode::kInvalidArgument,
                          "map outputs must not be null");
        }
        *data = bytes_.data();
        *size = bytes_.size();
        return Status::ok_status();
    }

    Status unmap() { return Status::ok_status(); }

    Result<NativeBufferHandle> export_dmabuf() const {
        return Result<NativeBufferHandle>(
            Status(StatusCode::kUnsupported,
                   "reference source CPU storage is not exportable"));
    }

private:
    std::vector<std::uint8_t> bytes_;
    std::string provider_id_;
};

Result<VideoFormat> make_reference_format() {
    const std::vector<PlaneLayout> planes{
        PlaneLayout(0U, 16U * 16U * 3U, 16U * 3U)};
    return VideoFormat::create(PixelFormat::kRgb24, 16, 16,
                               MemoryDomain::kCpu, planes);
}

Result<VideoProcessorConfig> make_processor_config(const VideoFormat& format) {
    return VideoProcessorConfig::create(format, format, 0, 0, 16, 16, 0);
}

Result<VideoEncoderConfig> make_encoder_config() {
    Result<TimeBase> time_base = TimeBase::create(1, 30);
    if (!time_base.ok()) {
        return Result<VideoEncoderConfig>(time_base.status());
    }
    return VideoEncoderConfig::create(
        CodecId::kReference, 16, 16, 30, 1, time_base.value(), 1, 1, 1, 0,
        RateControlMode::kConstantQuality, CodecProfile::kUnknown, 0, true);
}

SelectionConstraints reference_only() {
    return SelectionConstraints("reference");
}

SelectionPreferences no_preferences() {
    return SelectionPreferences(std::vector<std::string>(), false, false);
}

class ReferenceFrameSource : public MediaNode {
public:
    ReferenceFrameSource(MetricRegistry* metrics, const VideoFormat& format,
                         std::size_t* budget)
        : MediaNode("source"), output_("frame_output"), metrics_(metrics),
          format_(format), budget_(budget), next_pts_(0),
          time_base_(TimeBase::create(1, 30).value()) {}

    OutputPort<VideoFrame>& output() { return output_; }

protected:
    Status on_tick() override {
        try {
            if (pending_) {
                const Status status = output_.send(pending_);
                if (status.ok()) {
                    pending_.reset();
                }
                return status;
            }
            if (budget_ == NULL || *budget_ == 0U) {
                return Status::ok_status();
            }

            std::shared_ptr<BufferStorage> storage(
                new ReferenceCpuStorage(16U * 16U * 3U));
            Result<Buffer> created =
                Buffer::create(storage, format_.planes());
            if (!created.ok()) {
                return created.status();
            }
            Buffer buffer = created.take_value();
            Result<MappedRegion> mapped =
                buffer.map_plane(0U, MapMode::kReadWrite);
            if (!mapped.ok()) {
                return mapped.status();
            }
            for (std::size_t index = 0U; index < mapped.value().size();
                 ++index) {
                mapped.value().mutable_data()[index] =
                    static_cast<std::uint8_t>(
                        (next_pts_ + static_cast<std::int64_t>(index)) & 0xff);
            }
            Result<VideoFrame> frame = VideoFrame::create(
                buffer, format_, next_pts_, time_base_);
            if (!frame.ok()) {
                return frame.status();
            }
            pending_.reset(new VideoFrame(frame.take_value()));
            ++next_pts_;
            --(*budget_);
            Status status =
                metrics_->increment_counter("media.frames.allocated");
            if (!status.ok()) {
                return status;
            }
            status = output_.send(pending_);
            if (status.ok()) {
                pending_.reset();
            }
            return status;
        } catch (const std::bad_alloc&) {
            return allocation_failure();
        } catch (...) {
            return unexpected_failure();
        }
    }

    Status on_stop() override {
        try {
            if (!pending_) {
                return Status::ok_status();
            }
            const Status status = output_.send(pending_);
            if (status.ok()) {
                pending_.reset();
            }
            return status;
        } catch (const std::bad_alloc&) {
            return allocation_failure();
        } catch (...) {
            return unexpected_failure();
        }
    }

    Status on_reset() override {
        pending_.reset();
        next_pts_ = 0;
        return Status::ok_status();
    }

private:
    OutputPort<VideoFrame> output_;
    MetricRegistry* metrics_;
    VideoFormat format_;
    std::size_t* budget_;
    std::int64_t next_pts_;
    TimeBase time_base_;
    std::shared_ptr<const VideoFrame> pending_;
};

class ReferencePacketSink : public MediaNode {
public:
    explicit ReferencePacketSink(MetricRegistry* metrics)
        : MediaNode("sink"),
          input_("packet_input", 4U, OverflowPolicy::kBlock),
          metrics_(metrics) {}

    InputPort<MediaPacket>& input() { return input_; }

protected:
    Status consume_one() {
        try {
            Result<std::shared_ptr<const MediaPacket> > packet =
                input_.receive();
            if (!packet.ok()) {
                if (packet.status().code() == StatusCode::kNotFound) {
                    return metrics_->set_gauge("pipeline.queue.depth", 0.0);
                }
                return packet.status();
            }
            Status status =
                metrics_->increment_counter("media.frames.encoded");
            if (!status.ok()) {
                return status;
            }
            return metrics_->set_gauge(
                "pipeline.queue.depth",
                static_cast<double>(input_.queue_size()));
        } catch (const std::bad_alloc&) {
            return allocation_failure();
        } catch (...) {
            return unexpected_failure();
        }
    }

    Status on_tick() override { return consume_one(); }

    Status on_stop() override {
        if (input_.queue_size() == 0U) {
            return metrics_->set_gauge("pipeline.queue.depth", 0.0);
        }
        const Status status = consume_one();
        return status.ok() ? Status(StatusCode::kWouldBlock) : status;
    }

private:
    InputPort<MediaPacket> input_;
    MetricRegistry* metrics_;
};

}  // namespace

ReferenceMediaPlatform::ReferenceMediaPlatform()
    : options_(), command_handler_(&desired_), source_budget_(0U),
      initialized_(false) {}

ReferenceMediaPlatform::ReferenceMediaPlatform(
    const ReferenceBackendOptions& options)
    : options_(options), command_handler_(&desired_), source_budget_(0U),
      initialized_(false) {}

ReferenceMediaPlatform::~ReferenceMediaPlatform() {}

std::string ReferenceMediaPlatform::state_key(const char* suffix) const {
    return std::string("/pipelines/") + kPipelineId + suffix;
}

Status ReferenceMediaPlatform::publish_selection(
    const VideoFormat&, const std::string& provider_id) {
    Status status = actual_.set(state_key("/provider"), StateValue(provider_id));
    if (!status.ok()) {
        return status;
    }
    status = actual_.set(state_key("/pixel_format"), StateValue("rgb24"));
    if (!status.ok()) {
        return status;
    }
    return actual_.set(state_key("/memory_domain"), StateValue("cpu"));
}

Status ReferenceMediaPlatform::build_pipeline() {
    try {
        Result<VideoFormat> format_result = make_reference_format();
        if (!format_result.ok()) {
            return format_result.status();
        }
        const VideoFormat format = format_result.take_value();
        Result<VideoProcessorConfig> processor_config_result =
            make_processor_config(format);
        if (!processor_config_result.ok()) {
            return processor_config_result.status();
        }
        const VideoProcessorConfig processor_config =
            processor_config_result.take_value();
        Result<VideoEncoderConfig> encoder_config_result = make_encoder_config();
        if (!encoder_config_result.ok()) {
            return encoder_config_result.status();
        }
        const VideoEncoderConfig encoder_config =
            encoder_config_result.take_value();

        const VideoProcessorRequest processor_request(
            processor_config, std::vector<VideoProcessingOperation>(), 1U, 1U,
            true, reference_only(), no_preferences());
        const VideoEncoderRequest encoder_request(
            format, encoder_config, 1U, true, reference_only(),
            no_preferences());
        Result<ProcessorSelection> processor_selection =
            registry_.select_video_processor(processor_request);
        if (!processor_selection.ok()) {
            return processor_selection.status();
        }
        Result<EncoderSelection> encoder_selection =
            registry_.select_video_encoder(encoder_request);
        if (!encoder_selection.ok()) {
            return encoder_selection.status();
        }
        Result<std::unique_ptr<VideoProcessor> > created_processor =
            processor_selection.value().provider->create_video_processor();
        if (!created_processor.ok()) {
            return created_processor.status();
        }
        Result<std::unique_ptr<VideoEncoder> > created_encoder =
            encoder_selection.value().provider->create_video_encoder();
        if (!created_encoder.ok()) {
            return created_encoder.status();
        }

        std::unique_ptr<ReferenceFrameSource> source(
            new ReferenceFrameSource(&metrics_, format, &source_budget_));
        std::unique_ptr<VideoProcessorNode> processor(new VideoProcessorNode(
            "processor", created_processor.take_value(), processor_config, 4U));
        std::unique_ptr<VideoEncoderNode> encoder(new VideoEncoderNode(
            "encoder", created_encoder.take_value(), format, encoder_config, 4U));
        std::unique_ptr<ReferencePacketSink> sink(
            new ReferencePacketSink(&metrics_));

        Status status = connect(source->output(), processor->input());
        if (!status.ok()) {
            return status;
        }
        status = connect(processor->output(), encoder->input());
        if (!status.ok()) {
            return status;
        }
        status = connect(encoder->output(), sink->input());
        if (!status.ok()) {
            return status;
        }

        std::unique_ptr<MediaPipeline> pipeline(new MediaPipeline(kPipelineId));
        status = pipeline->add_node(
            std::unique_ptr<MediaNode>(source.release()));
        if (!status.ok()) {
            return status;
        }
        status = pipeline->add_node(
            std::unique_ptr<MediaNode>(processor.release()));
        if (!status.ok()) {
            return status;
        }
        status = pipeline->add_node(
            std::unique_ptr<MediaNode>(encoder.release()));
        if (!status.ok()) {
            return status;
        }
        status = pipeline->add_node(std::unique_ptr<MediaNode>(sink.release()));
        if (!status.ok()) {
            return status;
        }
        status = pipeline->connect("source", "processor");
        if (!status.ok()) {
            return status;
        }
        status = pipeline->connect("processor", "encoder");
        if (!status.ok()) {
            return status;
        }
        status = pipeline->connect("encoder", "sink");
        if (!status.ok()) {
            return status;
        }

        pipeline_ = std::move(pipeline);
        reconciler_.reset(
            new PipelineReconciler(pipeline_.get(), &desired_, &actual_));
        status = metrics_.increment_counter(
            "media.backend.instances.created", 2U);
        if (!status.ok()) {
            return status;
        }
        return publish_selection(
            format, processor_selection.value().negotiation.provider_id);
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    } catch (...) {
        return unexpected_failure();
    }
}

Status ReferenceMediaPlatform::initialize() {
    if (initialized_) {
        return Status::ok_status();
    }
    const std::shared_ptr<MediaBackendProvider> reference =
        create_reference_backend(options_);
    if (!reference) {
        return allocation_failure();
    }
    Status status = registry_.register_provider(reference);
    if (!status.ok()) {
        return status;
    }
    status = registry_.freeze();
    if (!status.ok()) {
        return status;
    }
    status = build_pipeline();
    if (!status.ok()) {
        health_.report("pipeline", HealthStatus::kError, status.message());
        return status;
    }
    initialized_ = true;
    return health_.report("pipeline", HealthStatus::kOk, "initialized");
}

Status ReferenceMediaPlatform::dispatch(const StartPipelineCommand& command) {
    return initialized_ ? command_handler_.handle(command)
                        : Status(StatusCode::kInvalidState,
                                 "platform is not initialized");
}

Status ReferenceMediaPlatform::dispatch(const StopPipelineCommand& command) {
    return initialized_ ? command_handler_.handle(command)
                        : Status(StatusCode::kInvalidState,
                                 "platform is not initialized");
}

Status ReferenceMediaPlatform::reconcile_once() {
    if (!initialized_ || !reconciler_) {
        return Status(StatusCode::kInvalidState,
                      "platform is not initialized");
    }
    const Status status = reconciler_->reconcile_once();
    if (status.code() == StatusCode::kWouldBlock) {
        health_.report("pipeline", HealthStatus::kOk, "pipeline is draining");
        return status;
    }
    if (!status.ok()) {
        health_.report("pipeline", HealthStatus::kError, status.message());
        return status;
    }
    health_.report("pipeline", HealthStatus::kOk,
                   "desired state converged");
    return Status::ok_status();
}

std::uint64_t ReferenceMediaPlatform::encoded_count() const {
    const Result<std::uint64_t> result =
        metrics_.counter("media.frames.encoded");
    return result.ok() ? result.value() : 0U;
}

Status ReferenceMediaPlatform::publish_runtime_failure(
    const Status& failure) {
    actual_.set(state_key(), StateValue("error"));
    actual_.set(state_key("/error/provider"),
                StateValue(failure.provider_id()));
    actual_.set(state_key("/error/operation"),
                StateValue(failure.operation()));
    actual_.set(state_key("/error/message"), StateValue(failure.message()));
    health_.report("pipeline", HealthStatus::kError, failure.message());
    return failure;
}

Status ReferenceMediaPlatform::tick(std::size_t count) {
    if (!initialized_ || !pipeline_) {
        return Status(StatusCode::kInvalidState,
                      "platform is not initialized");
    }
    if (pipeline_->state() != PipelineState::kRunning) {
        return Status(StatusCode::kInvalidState, "pipeline is not running");
    }
    if (count == 0U) {
        return Status::ok_status();
    }
    const std::uint64_t initial = encoded_count();
    if (count > std::numeric_limits<std::uint64_t>::max() - initial) {
        return Status(StatusCode::kInvalidArgument,
                      "requested tick count overflows encoded frame target");
    }
    const std::uint64_t target = initial + count;
    source_budget_ = count;
    const std::size_t maximum_size =
        std::numeric_limits<std::size_t>::max();
    const bool extra_overflows =
        options_.output_delay > maximum_size - 4U ||
        (options_.output_delay <= maximum_size - 4U &&
         options_.queue_capacity > maximum_size - options_.output_delay - 4U);
    const std::size_t extra =
        extra_overflows
            ? maximum_size
            : options_.queue_capacity + options_.output_delay + 4U;
    std::size_t turns = 0U;
    const std::size_t maximum_turns =
        extra == maximum_size || count > maximum_size - extra
            ? maximum_size
            : count + extra;
    while (encoded_count() < target && turns < maximum_turns) {
        const Status status = executor_.run(pipeline_.get(), 1U);
        if (!status.ok()) {
            return publish_runtime_failure(status);
        }
        ++turns;
    }
    if (encoded_count() != target) {
        return Status(StatusCode::kWouldBlock,
                      "pipeline did not produce the requested frame count");
    }
    health_.report("pipeline", HealthStatus::kOk, "running");
    return Status::ok_status();
}

Status ReferenceMediaPlatform::reset_pipeline() {
    if (!initialized_ || !pipeline_ ||
        pipeline_->state() != PipelineState::kError) {
        return Status(StatusCode::kInvalidState,
                      "only an errored pipeline can be reset");
    }
    pipeline_->stop();
    reconciler_.reset();
    pipeline_.reset();
    source_budget_ = 0U;
    Status status = build_pipeline();
    if (!status.ok()) {
        health_.report("pipeline", HealthStatus::kError, status.message());
        return status;
    }
    status = reconciler_->reconcile_once();
    if (!status.ok()) {
        return publish_runtime_failure(status);
    }
    health_.report("pipeline", HealthStatus::kOk, "pipeline rebuilt");
    return Status::ok_status();
}

Result<StateSnapshot> ReferenceMediaPlatform::query(
    const PipelineStateQuery& query_value) const {
    if (!pipeline_ || query_value.pipeline_id != pipeline_->id()) {
        return Result<StateSnapshot>(
            Status(StatusCode::kNotFound, "pipeline was not found"));
    }
    const StateSnapshot snapshot = actual_.snapshot();
    const Result<StateValue> state = snapshot.get(state_key());
    if (!state.ok()) {
        return Result<StateSnapshot>(state.status());
    }
    return Result<StateSnapshot>(snapshot);
}

const MetricRegistry& ReferenceMediaPlatform::metrics() const {
    return metrics_;
}

const HealthManager& ReferenceMediaPlatform::health() const { return health_; }

}  // namespace eavp
