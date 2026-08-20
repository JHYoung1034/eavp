#include "eavp/media/reference_backend.hpp"

#include <cstdint>
#include <deque>
#include <new>
#include <utility>
#include <vector>

namespace eavp {

namespace {

const char kReferenceProviderId[] = "reference";

Status invalid_state(const char*) {
    return Status(StatusCode::kInvalidState);
}

Status capability_mismatch(const char*) {
    return Status(StatusCode::kCapabilityMismatch);
}

Status would_block() {
    return Status(StatusCode::kWouldBlock);
}

Status end_of_stream() {
    return Status(StatusCode::kEndOfStream);
}

Status allocation_failure() {
    return Status(StatusCode::kResourceExhausted);
}

Status internal_failure() {
    return Status(StatusCode::kInternal);
}

Status device_lost() {
    return Status(StatusCode::kDeviceLost,
                  "reference backend injected device loss",
                  kReferenceProviderId, "submit", 0);
}

bool same_plane(const PlaneLayout& left, const PlaneLayout& right) {
    return left.offset == right.offset && left.size == right.size &&
           left.stride == right.stride;
}

bool same_format(const VideoFormat& left, const VideoFormat& right) {
    if (left.pixel_format() != right.pixel_format() ||
        left.width() != right.width() || left.height() != right.height() ||
        left.memory_domain() != right.memory_domain() ||
        left.color_range() != right.color_range() ||
        left.color_primaries() != right.color_primaries() ||
        left.transfer() != right.transfer() || left.matrix() != right.matrix() ||
        left.planes().size() != right.planes().size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left.planes().size(); ++index) {
        if (!same_plane(left.planes()[index], right.planes()[index])) {
            return false;
        }
    }
    return true;
}

bool is_identity_processor_config(const VideoProcessorConfig& config) {
    return config.input_format.memory_domain() == MemoryDomain::kCpu &&
           same_format(config.input_format, config.output_format) &&
           config.crop_x == 0 && config.crop_y == 0 &&
           config.crop_width == config.input_format.width() &&
           config.crop_height == config.input_format.height() &&
           config.rotation_degrees == 0;
}

bool is_reference_encoder_config(const VideoFormat& input,
                                 const VideoEncoderConfig& config) {
    return input.memory_domain() == MemoryDomain::kCpu &&
           config.codec == CodecId::kReference &&
           config.width == input.width() && config.height == input.height() &&
           config.rate_control == RateControlMode::kConstantQuality &&
           config.profile == CodecProfile::kUnknown && config.level_idc == 0;
}

std::vector<FormatMemoryDomain> reference_formats() {
    std::vector<FormatMemoryDomain> formats;
    formats.push_back(FormatMemoryDomain(
        PixelFormat::kRgb24, MemoryDomain::kCpu,
        std::vector<PlaneLayoutConstraint>{
            PlaneLayoutConstraint(1, 1, 3U, 1U, 1U, 1U, 1U)}));
    formats.push_back(FormatMemoryDomain(
        PixelFormat::kNv12, MemoryDomain::kCpu,
        std::vector<PlaneLayoutConstraint>{
            PlaneLayoutConstraint(1, 1, 1U, 1U, 1U, 1U, 1U),
            PlaneLayoutConstraint(1, 2, 1U, 1U, 1U, 1U, 1U)}));
    formats.push_back(FormatMemoryDomain(
        PixelFormat::kYuv420p, MemoryDomain::kCpu,
        std::vector<PlaneLayoutConstraint>{
            PlaneLayoutConstraint(1, 1, 1U, 1U, 1U, 1U, 1U),
            PlaneLayoutConstraint(2, 2, 1U, 1U, 1U, 1U, 1U),
            PlaneLayoutConstraint(2, 2, 1U, 1U, 1U, 1U, 1U)}));
    return formats;
}

ProviderCapability make_reference_capability() {
    const DimensionRange width(1, 4096, 1, 1);
    const DimensionRange height(1, 4096, 1, 1);
    const std::vector<FormatMemoryDomain> formats = reference_formats();
    const VideoProcessorCapability processor(
        width, height, width, height, formats, formats,
        std::vector<VideoProcessingOperation>(), true, true);
    const VideoEncoderCapability encoder(
        width, height, CodecId::kReference,
        std::vector<CodecProfile>{CodecProfile::kUnknown},
        std::vector<int>{0}, formats,
        std::vector<RateControlMode>{RateControlMode::kConstantQuality}, true);
    return ProviderCapability(
        kReferenceProviderId, "0.2", "reference-cpu",
        Status::ok_status(), ProviderKind::kReference,
        std::vector<VideoProcessorCapability>{processor},
        std::vector<VideoEncoderCapability>{encoder}, 0,
        std::vector<std::string>());
}

bool can_submit(BackendState state) {
    return state == BackendState::kConfigured ||
           state == BackendState::kRunning;
}

bool can_receive(BackendState state) {
    return state == BackendState::kConfigured ||
           state == BackendState::kRunning ||
           state == BackendState::kDraining ||
           state == BackendState::kStopped;
}

class ReferenceProcessor : public VideoProcessor {
public:
    explicit ReferenceProcessor(const ReferenceBackendOptions& options)
        : options_(options),
          state_(BackendState::kCreated),
          successful_submissions_(0U) {}

    BackendState state() const { return state_; }

    Status configure(const VideoProcessorConfig& config) {
        const Status affinity = bind_to_current_thread();
        if (!affinity.ok()) {
            return affinity;
        }
        if (state_ != BackendState::kCreated) {
            return invalid_state("configure");
        }
        if (!is_identity_processor_config(config)) {
            return capability_mismatch(
                "reference processor supports only identical CPU formats");
        }
        try {
            config_.reset(new VideoProcessorConfig(config));
        } catch (const std::bad_alloc&) {
            return allocation_failure();
        }
        state_ = BackendState::kConfigured;
        return Status::ok_status();
    }

    Status submit(const std::shared_ptr<const VideoFrame>& frame) {
        const Status affinity = verify_current_thread();
        if (!affinity.ok()) {
            return affinity;
        }
        if (!can_submit(state_)) {
            return invalid_state("submit");
        }
        if (!frame) {
            return Status(StatusCode::kInvalidArgument);
        }
        if (!same_format(frame->format(), config_->input_format)) {
            return capability_mismatch(
                "reference processor input frame format does not match configuration");
        }
        if (options_.device_lost_after_submissions != 0U &&
            successful_submissions_ >=
                options_.device_lost_after_submissions) {
            try {
                const Status status = device_lost();
                queue_.clear();
                state_ = BackendState::kError;
                return status;
            } catch (const std::bad_alloc&) {
                queue_.clear();
                state_ = BackendState::kError;
                return allocation_failure();
            } catch (...) {
                queue_.clear();
                state_ = BackendState::kError;
                return internal_failure();
            }
        }
        if (queue_.size() >= options_.queue_capacity) {
            return would_block();
        }
        try {
            queue_.push_back(frame);
        } catch (const std::bad_alloc&) {
            return allocation_failure();
        }
        ++successful_submissions_;
        state_ = BackendState::kRunning;
        return Status::ok_status();
    }

    Result<std::shared_ptr<const VideoFrame> > receive() {
        const Status affinity = verify_current_thread();
        if (!affinity.ok()) {
            return Result<std::shared_ptr<const VideoFrame> >(affinity);
        }
        if (!can_receive(state_)) {
            return Result<std::shared_ptr<const VideoFrame> >(
                invalid_state("receive"));
        }
        if (state_ == BackendState::kStopped) {
            return Result<std::shared_ptr<const VideoFrame> >(end_of_stream());
        }
        if (!queue_.empty()) {
            std::shared_ptr<const VideoFrame> frame = queue_.front();
            try {
                Result<std::shared_ptr<const VideoFrame> > output(frame);
                queue_.pop_front();
                return output;
            } catch (const std::bad_alloc&) {
                return Result<std::shared_ptr<const VideoFrame> >(
                    allocation_failure());
            } catch (...) {
                return Result<std::shared_ptr<const VideoFrame> >(
                    internal_failure());
            }
        }
        if (state_ == BackendState::kDraining) {
            state_ = BackendState::kStopped;
            return Result<std::shared_ptr<const VideoFrame> >(end_of_stream());
        }
        return Result<std::shared_ptr<const VideoFrame> >(would_block());
    }

    Status begin_drain() {
        const Status affinity = verify_current_thread();
        if (!affinity.ok()) {
            return affinity;
        }
        if (state_ == BackendState::kStopped ||
            state_ == BackendState::kDraining) {
            return Status::ok_status();
        }
        if (state_ != BackendState::kConfigured &&
            state_ != BackendState::kRunning) {
            return invalid_state("drain");
        }
        state_ = BackendState::kDraining;
        return Status::ok_status();
    }

    Status reset() {
        const Status affinity = verify_current_thread();
        if (!affinity.ok()) {
            return affinity;
        }
        queue_.clear();
        config_.reset();
        successful_submissions_ = 0U;
        state_ = BackendState::kCreated;
        return Status::ok_status();
    }

private:
    ReferenceBackendOptions options_;
    BackendState state_;
    std::size_t successful_submissions_;
    std::unique_ptr<VideoProcessorConfig> config_;
    std::deque<std::shared_ptr<const VideoFrame> > queue_;
};

class ReferenceEncoder : public VideoEncoder {
public:
    explicit ReferenceEncoder(const ReferenceBackendOptions& options)
        : options_(options),
          state_(BackendState::kCreated),
          successful_submissions_(0U) {}

    BackendState state() const { return state_; }

    Status configure(const VideoFormat& input,
                     const VideoEncoderConfig& config) {
        const Status affinity = bind_to_current_thread();
        if (!affinity.ok()) {
            return affinity;
        }
        if (state_ != BackendState::kCreated) {
            return invalid_state("configure");
        }
        if (!is_reference_encoder_config(input, config)) {
            return capability_mismatch(
                "reference encoder supports only its CPU reference codec");
        }
        try {
            input_format_.reset(new VideoFormat(input));
            config_.reset(new VideoEncoderConfig(config));
        } catch (const std::bad_alloc&) {
            input_format_.reset();
            config_.reset();
            return allocation_failure();
        }
        state_ = BackendState::kConfigured;
        return Status::ok_status();
    }

    Status submit(const std::shared_ptr<const VideoFrame>& frame) {
        const Status affinity = verify_current_thread();
        if (!affinity.ok()) {
            return affinity;
        }
        if (!can_submit(state_)) {
            return invalid_state("submit");
        }
        if (!frame) {
            return Status(StatusCode::kInvalidArgument);
        }
        if (!same_format(frame->format(), *input_format_)) {
            return capability_mismatch(
                "reference encoder input frame format does not match configuration");
        }
        if (options_.device_lost_after_submissions != 0U &&
            successful_submissions_ >=
                options_.device_lost_after_submissions) {
            try {
                const Status status = device_lost();
                queue_.clear();
                state_ = BackendState::kError;
                return status;
            } catch (const std::bad_alloc&) {
                queue_.clear();
                state_ = BackendState::kError;
                return allocation_failure();
            } catch (...) {
                queue_.clear();
                state_ = BackendState::kError;
                return internal_failure();
            }
        }
        if (queue_.size() >= options_.queue_capacity) {
            return would_block();
        }
        try {
            queue_.push_back(frame);
        } catch (const std::bad_alloc&) {
            return allocation_failure();
        }
        ++successful_submissions_;
        state_ = BackendState::kRunning;
        return Status::ok_status();
    }

    Result<std::shared_ptr<const MediaPacket> > receive() {
        const Status affinity = verify_current_thread();
        if (!affinity.ok()) {
            return Result<std::shared_ptr<const MediaPacket> >(affinity);
        }
        if (!can_receive(state_)) {
            return Result<std::shared_ptr<const MediaPacket> >(
                invalid_state("receive"));
        }
        if (state_ == BackendState::kStopped) {
            return Result<std::shared_ptr<const MediaPacket> >(end_of_stream());
        }
        if (!queue_.empty() &&
            (state_ == BackendState::kDraining ||
             queue_.size() > options_.output_delay)) {
            try {
                Result<std::shared_ptr<const MediaPacket> > packet =
                    encode(*queue_.front());
                if (!packet.ok()) {
                    state_ = BackendState::kError;
                    return packet;
                }
                queue_.pop_front();
                return packet;
            } catch (const std::bad_alloc&) {
                state_ = BackendState::kError;
                return Result<std::shared_ptr<const MediaPacket> >(
                    allocation_failure());
            } catch (...) {
                state_ = BackendState::kError;
                return Result<std::shared_ptr<const MediaPacket> >(
                    internal_failure());
            }
        }
        if (state_ == BackendState::kDraining) {
            state_ = BackendState::kStopped;
            return Result<std::shared_ptr<const MediaPacket> >(end_of_stream());
        }
        return Result<std::shared_ptr<const MediaPacket> >(would_block());
    }

    Status begin_drain() {
        const Status affinity = verify_current_thread();
        if (!affinity.ok()) {
            return affinity;
        }
        if (state_ == BackendState::kStopped ||
            state_ == BackendState::kDraining) {
            return Status::ok_status();
        }
        if (state_ != BackendState::kConfigured &&
            state_ != BackendState::kRunning) {
            return invalid_state("drain");
        }
        state_ = BackendState::kDraining;
        return Status::ok_status();
    }

    Status reset() {
        const Status affinity = verify_current_thread();
        if (!affinity.ok()) {
            return affinity;
        }
        queue_.clear();
        input_format_.reset();
        config_.reset();
        successful_submissions_ = 0U;
        state_ = BackendState::kCreated;
        return Status::ok_status();
    }

private:
    Result<std::shared_ptr<const MediaPacket> > encode(
        const VideoFrame& frame) const {
        Result<MappedRegion> input =
            frame.buffer().map_plane(0U, MapMode::kReadOnly);
        if (!input.ok()) {
            return Result<std::shared_ptr<const MediaPacket> >(input.status());
        }
        std::uint32_t checksum = 0U;
        for (std::size_t index = 0U; index < input.value().size(); ++index) {
            checksum += input.value().data()[index];
        }

        Result<Buffer> allocated = Buffer::allocate(16U);
        if (!allocated.ok()) {
            return Result<std::shared_ptr<const MediaPacket> >(
                allocated.status());
        }
        Buffer payload = allocated.take_value();
        Result<MappedRegion> output =
            payload.map_plane(0U, MapMode::kReadWrite);
        if (!output.ok()) {
            return Result<std::shared_ptr<const MediaPacket> >(output.status());
        }

        // EAVP magic 和简单加和校验只定义测试格式，不能被当作标准码流。
        std::uint8_t* bytes = output.value().mutable_data();
        bytes[0] = 0x45U;
        bytes[1] = 0x41U;
        bytes[2] = 0x56U;
        bytes[3] = 0x50U;
        const std::uint64_t pts = static_cast<std::uint64_t>(frame.pts());
        for (std::size_t index = 0U; index < 8U; ++index) {
            bytes[4U + index] = static_cast<std::uint8_t>(
                pts >> (56U - static_cast<unsigned int>(index) * 8U));
        }
        for (std::size_t index = 0U; index < 4U; ++index) {
            bytes[12U + index] = static_cast<std::uint8_t>(
                checksum >> (24U - static_cast<unsigned int>(index) * 8U));
        }

        CodecConfigData codec_config;
        Result<MediaPacket> created = MediaPacket::create(
            payload, CodecId::kReference, EncodedStreamFormat::kReference, 0,
            frame.pts(), frame.pts(), 1, frame.time_base(), true,
            codec_config);
        if (!created.ok()) {
            return Result<std::shared_ptr<const MediaPacket> >(
                created.status());
        }
        try {
            std::shared_ptr<const MediaPacket> packet(
                new MediaPacket(created.take_value()));
            return Result<std::shared_ptr<const MediaPacket> >(packet);
        } catch (const std::bad_alloc&) {
            return Result<std::shared_ptr<const MediaPacket> >(
                allocation_failure());
        }
    }

    ReferenceBackendOptions options_;
    BackendState state_;
    std::size_t successful_submissions_;
    std::unique_ptr<VideoFormat> input_format_;
    std::unique_ptr<VideoEncoderConfig> config_;
    std::deque<std::shared_ptr<const VideoFrame> > queue_;
};

class ReferenceBackendProvider : public MediaBackendProvider {
public:
    explicit ReferenceBackendProvider(const ReferenceBackendOptions& options)
        : options_(options) {}

    Result<ProviderCapability> probe() const {
        try {
            return Result<ProviderCapability>(make_reference_capability());
        } catch (const std::bad_alloc&) {
            return Result<ProviderCapability>(allocation_failure());
        }
    }

    Result<std::unique_ptr<VideoProcessor> > create_video_processor() const {
        try {
            std::unique_ptr<VideoProcessor> processor(
                new ReferenceProcessor(options_));
            return Result<std::unique_ptr<VideoProcessor> >(
                std::move(processor));
        } catch (const std::bad_alloc&) {
            return Result<std::unique_ptr<VideoProcessor> >(
                allocation_failure());
        }
    }

    Result<std::unique_ptr<VideoEncoder> > create_video_encoder() const {
        try {
            std::unique_ptr<VideoEncoder> encoder(
                new ReferenceEncoder(options_));
            return Result<std::unique_ptr<VideoEncoder> >(std::move(encoder));
        } catch (const std::bad_alloc&) {
            return Result<std::unique_ptr<VideoEncoder> >(
                allocation_failure());
        }
    }

private:
    ReferenceBackendOptions options_;
};

}  // namespace

std::shared_ptr<MediaBackendProvider> create_reference_backend(
    const ReferenceBackendOptions& options) noexcept {
    try {
        return std::shared_ptr<MediaBackendProvider>(
            new ReferenceBackendProvider(options));
    } catch (const std::bad_alloc&) {
        return std::shared_ptr<MediaBackendProvider>();
    }
}

}  // namespace eavp
