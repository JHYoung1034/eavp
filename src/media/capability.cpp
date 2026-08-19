#include "eavp/media/capability.hpp"

#include <cstdint>
#include <new>

namespace eavp {

namespace {

Status mismatch(const std::string& message) {
    return Status(StatusCode::kCapabilityMismatch, message);
}

bool contains_format(const std::vector<FormatMemoryDomain>& formats,
                     PixelFormat pixel_format, MemoryDomain memory_domain) {
    for (std::size_t index = 0; index < formats.size(); ++index) {
        if (formats[index].pixel_format() == pixel_format &&
            formats[index].memory_domain() == memory_domain) {
            return true;
        }
    }
    return false;
}

template <typename T>
bool contains_value(const std::vector<T>& values, T value) {
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (values[index] == value) {
            return true;
        }
    }
    return false;
}

const char* operation_name(VideoProcessingOperation operation) {
    switch (operation) {
        case VideoProcessingOperation::kScaling:
            return "scaling";
        case VideoProcessingOperation::kCropping:
            return "cropping";
        case VideoProcessingOperation::kRotation:
            return "rotation";
        case VideoProcessingOperation::kFormatConversion:
            return "format conversion";
    }
    return "unknown processing operation";
}

bool requires_operation(const VideoProcessorConfig& config,
                        VideoProcessingOperation operation) {
    switch (operation) {
        case VideoProcessingOperation::kScaling:
            return config.crop_width != config.output_format.width() ||
                   config.crop_height != config.output_format.height();
        case VideoProcessingOperation::kCropping:
            return config.crop_x != 0 || config.crop_y != 0 ||
                   config.crop_width != config.input_format.width() ||
                   config.crop_height != config.input_format.height();
        case VideoProcessingOperation::kRotation:
            return config.rotation_degrees != 0;
        case VideoProcessingOperation::kFormatConversion:
            return config.input_format.pixel_format() !=
                   config.output_format.pixel_format();
    }
    return false;
}

Status check_operation(const std::vector<VideoProcessingOperation>& supported,
                       VideoProcessingOperation operation) {
    if (!contains_value(supported, operation)) {
        return mismatch(std::string("video processor does not support required ") +
                        operation_name(operation));
    }
    return Status::ok_status();
}

int preferred_provider_rank(const std::vector<std::string>& preferred,
                            const std::string& provider_id) {
    for (std::size_t index = 0; index < preferred.size(); ++index) {
        if (preferred[index] == provider_id) {
            return static_cast<int>(index);
        }
    }
    return static_cast<int>(preferred.size()) + 1;
}

int provider_kind_rank(ProviderKind kind, bool prefer_hardware) {
    if (!prefer_hardware) {
        return 0;
    }
    switch (kind) {
        case ProviderKind::kHardware:
            return 0;
        case ProviderKind::kSoftware:
            return 1;
        case ProviderKind::kReference:
            return 2;
    }
    return 3;
}

}  // namespace

bool DimensionRange::valid() const {
    if (minimum_ <= 0 || maximum_ < minimum_ || step_ <= 0 ||
        alignment_ <= 0) {
        return false;
    }
    const std::int64_t aligned_minimum =
        ((static_cast<std::int64_t>(minimum_) + alignment_ - 1) /
         alignment_) *
        alignment_;
    return aligned_minimum <= maximum_;
}

bool DimensionRange::contains(int value) const {
    if (!valid() || value < minimum_ || value > maximum_ ||
        (value - minimum_) % step_ != 0) {
        return false;
    }
    // 1080 等逻辑尺寸不必自身整除硬件对齐；对齐后的存储尺寸必须仍在能力上限内。
    const std::int64_t aligned_value =
        ((static_cast<std::int64_t>(value) + alignment_ - 1) / alignment_) *
        alignment_;
    return aligned_value <= maximum_;
}

Status VideoProcessorCapability::match(
    const VideoProcessorRequest& request) const {
    if (!input_width_.valid() || !input_height_.valid() ||
        !output_width_.valid() || !output_height_.valid()) {
        return mismatch("video processor capability has an invalid dimension range");
    }

    const VideoProcessorConfig& config = request.config();
    if (!input_width_.contains(config.input_format.width()) ||
        !input_height_.contains(config.input_format.height())) {
        return mismatch("video processor input dimensions are outside capability");
    }
    if (!output_width_.contains(config.output_format.width()) ||
        !output_height_.contains(config.output_format.height())) {
        return mismatch("video processor output dimensions are outside capability");
    }
    if (!contains_format(input_formats_, config.input_format.pixel_format(),
                         config.input_format.memory_domain())) {
        return mismatch(
            "video processor input pixel format or memory domain is unsupported");
    }
    if (!contains_format(output_formats_, config.output_format.pixel_format(),
                         config.output_format.memory_domain())) {
        return mismatch(
            "video processor output pixel format or memory domain is unsupported");
    }

    for (std::size_t index = 0; index < request.required_operations().size();
         ++index) {
        const Status status =
            check_operation(operations_, request.required_operations()[index]);
        if (!status.ok()) {
            return status;
        }
    }
    const VideoProcessingOperation inferred_operations[] = {
        VideoProcessingOperation::kScaling,
        VideoProcessingOperation::kCropping,
        VideoProcessingOperation::kRotation,
        VideoProcessingOperation::kFormatConversion};
    for (std::size_t index = 0;
         index < sizeof(inferred_operations) / sizeof(inferred_operations[0]);
         ++index) {
        if (requires_operation(config, inferred_operations[index])) {
            const Status status =
                check_operation(operations_, inferred_operations[index]);
            if (!status.ok()) {
                return status;
            }
        }
    }
    if (request.require_zero_copy() && !zero_copy_) {
        return mismatch("video processor does not support required zero-copy");
    }
    return Status::ok_status();
}

Status VideoEncoderCapability::match(
    const VideoEncoderRequest& request) const {
    if (!width_.valid() || !height_.valid()) {
        return mismatch("video encoder capability has an invalid dimension range");
    }

    const VideoEncoderConfig& config = request.config();
    if (!width_.contains(config.width) || !height_.contains(config.height)) {
        return mismatch("video encoder dimensions are outside capability");
    }
    if (request.input_format().width() != config.width ||
        request.input_format().height() != config.height) {
        return mismatch(
            "video encoder input dimensions do not match required configuration");
    }
    if (!contains_format(input_formats_, request.input_format().pixel_format(),
                         request.input_format().memory_domain())) {
        return mismatch(
            "video encoder input pixel format or memory domain is unsupported");
    }
    if (config.codec != codec_) {
        return mismatch("video encoder codec is unsupported");
    }
    if (!contains_value(profiles_, config.profile)) {
        return mismatch("video encoder codec/profile combination is unsupported");
    }
    if (!contains_value(levels_, config.level_idc)) {
        return mismatch("video encoder level is unsupported");
    }
    if (!contains_value(rate_control_modes_, config.rate_control)) {
        return mismatch("video encoder rate control mode is unsupported");
    }
    if (request.require_zero_copy() && !zero_copy_) {
        return mismatch("video encoder does not support required zero-copy");
    }
    return Status::ok_status();
}

bool ProviderPreferenceScore::operator<(
    const ProviderPreferenceScore& other) const {
    if (provider_rank_ != other.provider_rank_) {
        return provider_rank_ < other.provider_rank_;
    }
    if (kind_rank_ != other.kind_rank_) {
        return kind_rank_ < other.kind_rank_;
    }
    if (zero_copy_rank_ != other.zero_copy_rank_) {
        return zero_copy_rank_ < other.zero_copy_rank_;
    }
    return provider_id_ < other.provider_id_;
}

Status ProviderCapability::match(const VideoProcessorRequest& request) const {
    if (!availability_status_.ok()) {
        return availability_status_;
    }
    if (processor_capabilities_.empty()) {
        return mismatch("provider has no video processor capability");
    }
    Status first_mismatch = processor_capabilities_[0].match(request);
    if (first_mismatch.ok()) {
        return first_mismatch;
    }
    for (std::size_t index = 1; index < processor_capabilities_.size();
         ++index) {
        const Status status = processor_capabilities_[index].match(request);
        if (status.ok()) {
            return status;
        }
    }
    return first_mismatch;
}

Status ProviderCapability::match(const VideoEncoderRequest& request) const {
    if (!availability_status_.ok()) {
        return availability_status_;
    }
    if (encoder_capabilities_.empty()) {
        return mismatch("provider has no video encoder capability");
    }
    Status first_mismatch = encoder_capabilities_[0].match(request);
    if (first_mismatch.ok()) {
        return first_mismatch;
    }
    for (std::size_t index = 1; index < encoder_capabilities_.size();
         ++index) {
        const Status status = encoder_capabilities_[index].match(request);
        if (status.ok()) {
            return status;
        }
    }
    return first_mismatch;
}

Result<VideoProcessorNegotiation> ProviderCapability::negotiate(
    const VideoProcessorRequest& request) const {
    const Status status = match(request);
    if (!status.ok()) {
        return Result<VideoProcessorNegotiation>(status);
    }
    try {
        return Result<VideoProcessorNegotiation>(VideoProcessorNegotiation(
            provider_id_, request.config(), request.config().output_format, false));
    } catch (const std::bad_alloc&) {
        return Result<VideoProcessorNegotiation>(Status(
            StatusCode::kResourceExhausted,
            "failed to create video processor negotiation result"));
    }
}

Result<VideoEncoderNegotiation> ProviderCapability::negotiate(
    const VideoEncoderRequest& request) const {
    const Status status = match(request);
    if (!status.ok()) {
        return Result<VideoEncoderNegotiation>(status);
    }
    try {
        return Result<VideoEncoderNegotiation>(VideoEncoderNegotiation(
            provider_id_, request.config(), request.input_format(), false));
    } catch (const std::bad_alloc&) {
        return Result<VideoEncoderNegotiation>(Status(
            StatusCode::kResourceExhausted,
            "failed to create video encoder negotiation result"));
    }
}

ProviderPreferenceScore ProviderCapability::preference_score(
    const SelectionPreferences& preferences, bool zero_copy) const {
    return ProviderPreferenceScore(
        preferred_provider_rank(preferences.preferred_provider_ids(),
                                provider_id_),
        provider_kind_rank(kind_, preferences.prefer_hardware()),
        preferences.prefer_zero_copy() && !zero_copy ? 1 : 0, provider_id_);
}

}  // namespace eavp
