#include "eavp/media/capability.hpp"

#include <cstdint>
#include <limits>
#include <new>

namespace eavp {

namespace {

Status mismatch(const std::string& message) {
    return Status(StatusCode::kCapabilityMismatch, message);
}

bool checked_add(std::size_t left, std::size_t right, std::size_t* result) {
    if (left > std::numeric_limits<std::size_t>::max() - right) {
        return false;
    }
    *result = left + right;
    return true;
}

bool checked_multiply(std::size_t left, std::size_t right,
                      std::size_t* result) {
    if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    *result = left * right;
    return true;
}

bool align_up(std::size_t value, std::size_t alignment, std::size_t* result) {
    if (alignment == 0U) {
        return false;
    }
    const std::size_t remainder = value % alignment;
    if (remainder == 0U) {
        *result = value;
        return true;
    }
    return checked_add(value, alignment - remainder, result);
}

std::size_t greatest_common_divisor(std::size_t left, std::size_t right) {
    while (right != 0U) {
        const std::size_t remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}

bool least_common_multiple(std::size_t left, std::size_t right,
                           std::size_t* result) {
    const std::size_t divisor = greatest_common_divisor(left, right);
    return divisor != 0U && checked_multiply(left / divisor, right, result);
}

const FormatMemoryDomain* find_format(
    const std::vector<FormatMemoryDomain>& formats, PixelFormat pixel_format,
    MemoryDomain memory_domain) {
    for (std::size_t index = 0; index < formats.size(); ++index) {
        if (formats[index].pixel_format() == pixel_format &&
            formats[index].memory_domain() == memory_domain) {
            return &formats[index];
        }
    }
    return NULL;
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

bool same_plane_layout(const PlaneLayout& left, const PlaneLayout& right) {
    return left.offset == right.offset && left.size == right.size &&
           left.stride == right.stride;
}

bool same_video_format(const VideoFormat& left, const VideoFormat& right) {
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
        if (!same_plane_layout(left.planes()[index], right.planes()[index])) {
            return false;
        }
    }
    return true;
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
        case VideoProcessingOperation::kScaling: {
            const bool swaps_axes = config.rotation_degrees == 90 ||
                                    config.rotation_degrees == 270;
            const int rotated_width =
                swaps_axes ? config.crop_height : config.crop_width;
            const int rotated_height =
                swaps_axes ? config.crop_width : config.crop_height;
            return rotated_width != config.output_format.width() ||
                   rotated_height != config.output_format.height();
        }
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

bool plane_address_is_proven(const PlaneLayoutConstraint& constraint,
                             const PlaneLayout& plane,
                             std::size_t address_alignment) {
    if (constraint.address_alignment() == 1U) {
        return true;
    }
    return address_alignment != 0U &&
           address_alignment % constraint.address_alignment() == 0U &&
           plane.offset % constraint.address_alignment() == 0U;
}

bool layout_matches(const VideoFormat& format,
                    const FormatMemoryDomain& format_capability,
                    const DimensionRange& width_range,
                    const DimensionRange& height_range,
                    std::size_t address_alignment) {
    if (!format_capability.valid() ||
        format.planes().size() != format_capability.plane_constraints().size() ||
        !width_range.contains(format.width()) ||
        !height_range.contains(format.height())) {
        return false;
    }

    std::size_t storage_width = 0U;
    std::size_t storage_height = 0U;
    if (!align_up(static_cast<std::size_t>(format.width()),
                  static_cast<std::size_t>(width_range.alignment()),
                  &storage_width) ||
        !align_up(static_cast<std::size_t>(format.height()),
                  static_cast<std::size_t>(height_range.alignment()),
                  &storage_height)) {
        return false;
    }

    for (std::size_t index = 0; index < format.planes().size(); ++index) {
        const PlaneLayout& plane = format.planes()[index];
        const PlaneLayoutConstraint& constraint =
            format_capability.plane_constraints()[index];
        if (plane.offset >
            std::numeric_limits<std::size_t>::max() - plane.size) {
            return false;
        }
        const std::size_t plane_end = plane.offset + plane.size;
        for (std::size_t previous_index = 0; previous_index < index;
             ++previous_index) {
            const PlaneLayout& previous = format.planes()[previous_index];
            if (previous.offset >
                std::numeric_limits<std::size_t>::max() - previous.size) {
                return false;
            }
            const std::size_t previous_end = previous.offset + previous.size;
            if (plane.offset < previous_end && previous.offset < plane_end) {
                return false;
            }
        }
        std::size_t samples = 0U;
        std::size_t minimum_stride = 0U;
        std::size_t aligned_stride = 0U;
        std::size_t required_size = 0U;
        const std::size_t horizontal =
            static_cast<std::size_t>(constraint.horizontal_subsampling());
        const std::size_t vertical =
            static_cast<std::size_t>(constraint.vertical_subsampling());
        if (!checked_add(storage_width, horizontal - 1U, &samples)) {
            return false;
        }
        samples /= horizontal;
        if (!checked_multiply(samples, constraint.bytes_per_sample(),
                              &minimum_stride) ||
            !align_up(minimum_stride, constraint.stride_alignment(),
                      &aligned_stride) ||
            plane.stride < aligned_stride ||
            plane.stride % constraint.stride_alignment() != 0U ||
            plane.offset % constraint.offset_alignment() != 0U ||
            plane.size % constraint.size_alignment() != 0U ||
            !plane_address_is_proven(constraint, plane, address_alignment)) {
            return false;
        }
        std::size_t rows = 0U;
        if (!checked_add(storage_height, vertical - 1U, &rows)) {
            return false;
        }
        rows /= vertical;
        if (!checked_multiply(plane.stride, rows, &required_size) ||
            plane.size < required_size) {
            return false;
        }
    }
    return true;
}

Result<VideoFormat> make_actual_format(
    const VideoFormat& requested, const FormatMemoryDomain& format_capability,
    const DimensionRange& width_range, const DimensionRange& height_range) {
    if (!format_capability.valid()) {
        return Result<VideoFormat>(
            mismatch("capability plane layout constraints are invalid"));
    }
    std::size_t storage_width = 0U;
    std::size_t storage_height = 0U;
    if (!align_up(static_cast<std::size_t>(requested.width()),
                  static_cast<std::size_t>(width_range.alignment()),
                  &storage_width) ||
        !align_up(static_cast<std::size_t>(requested.height()),
                  static_cast<std::size_t>(height_range.alignment()),
                  &storage_height)) {
        return Result<VideoFormat>(Status(
            StatusCode::kResourceExhausted,
            "video format storage extent overflows capability representation"));
    }

    std::vector<PlaneLayout> planes;
    try {
        planes.reserve(format_capability.plane_constraints().size());
        std::size_t next_offset = 0U;
        for (std::size_t index = 0;
             index < format_capability.plane_constraints().size(); ++index) {
            const PlaneLayoutConstraint& constraint =
                format_capability.plane_constraints()[index];
            const std::size_t horizontal =
                static_cast<std::size_t>(constraint.horizontal_subsampling());
            const std::size_t vertical =
                static_cast<std::size_t>(constraint.vertical_subsampling());
            std::size_t samples = 0U;
            std::size_t stride = 0U;
            std::size_t rows = 0U;
            std::size_t size = 0U;
            std::size_t offset_alignment = 0U;
            std::size_t offset = 0U;
            if (!checked_add(storage_width, horizontal - 1U, &samples)) {
                throw std::bad_alloc();
            }
            samples /= horizontal;
            if (!checked_multiply(samples, constraint.bytes_per_sample(),
                                  &stride) ||
                !align_up(stride, constraint.stride_alignment(), &stride) ||
                !checked_add(storage_height, vertical - 1U, &rows)) {
                throw std::bad_alloc();
            }
            rows /= vertical;
            if (!checked_multiply(stride, rows, &size) ||
                !align_up(size, constraint.size_alignment(), &size) ||
                !least_common_multiple(constraint.offset_alignment(),
                                       constraint.address_alignment(),
                                       &offset_alignment) ||
                !align_up(next_offset, offset_alignment, &offset) ||
                !checked_add(offset, size, &next_offset)) {
                throw std::bad_alloc();
            }
            planes.push_back(PlaneLayout(offset, size, stride));
        }
    } catch (const std::bad_alloc&) {
        return Result<VideoFormat>(Status(
            StatusCode::kResourceExhausted,
            "failed to create aligned video plane layout"));
    }

    return VideoFormat::create(
        requested.pixel_format(), requested.width(), requested.height(),
        requested.memory_domain(), planes, requested.color_range(),
        requested.color_primaries(), requested.transfer(), requested.matrix());
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

Status required_provider_status(const std::string& provider_id,
                                const SelectionConstraints& constraints) {
    if (!constraints.required_provider_id().empty() &&
        constraints.required_provider_id() != provider_id) {
        return mismatch("candidate is not the explicitly required provider");
    }
    return Status::ok_status();
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
    // 对齐描述可见尺寸对应的存储 extent；1080 可由 1088 的对齐存储承载。
    const std::int64_t aligned_value =
        ((static_cast<std::int64_t>(value) + alignment_ - 1) / alignment_) *
        alignment_;
    return aligned_value <= maximum_;
}

bool PlaneLayoutConstraint::valid() const {
    return horizontal_subsampling_ > 0 && vertical_subsampling_ > 0 &&
           bytes_per_sample_ > 0U && stride_alignment_ > 0U &&
           offset_alignment_ > 0U && size_alignment_ > 0U &&
           address_alignment_ > 0U;
}

bool FormatMemoryDomain::valid() const {
    std::size_t expected_plane_count = 0U;
    switch (pixel_format_) {
        case PixelFormat::kRgb24:
            expected_plane_count = 1U;
            break;
        case PixelFormat::kNv12:
            expected_plane_count = 2U;
            break;
        case PixelFormat::kYuv420p:
            expected_plane_count = 3U;
            break;
        case PixelFormat::kUnknown:
            return false;
    }
    if (plane_constraints_.size() != expected_plane_count) {
        return false;
    }
    for (std::size_t index = 0; index < plane_constraints_.size(); ++index) {
        if (!plane_constraints_[index].valid()) {
            return false;
        }
    }
    return true;
}

Status VideoProcessorCapability::match(
    const VideoProcessorRequest& request) const {
    if (!input_width_.valid() || !input_height_.valid() ||
        !output_width_.valid() || !output_height_.valid()) {
        return mismatch("video processor capability has an invalid dimension range");
    }

    const VideoProcessorConfig& config = request.config();
    if (requires_identical_formats_ &&
        !same_video_format(config.input_format, config.output_format)) {
        return mismatch(
            "video processor requires identical input and output formats");
    }
    if (!input_width_.contains(config.input_format.width()) ||
        !input_height_.contains(config.input_format.height())) {
        return mismatch("video processor input dimensions are outside capability");
    }
    if (!output_width_.contains(config.output_format.width()) ||
        !output_height_.contains(config.output_format.height())) {
        return mismatch("video processor output dimensions are outside capability");
    }
    const FormatMemoryDomain* input_format = find_format(
        input_formats_, config.input_format.pixel_format(),
        config.input_format.memory_domain());
    if (input_format == NULL) {
        return mismatch(
            "video processor input pixel format or memory domain is unsupported");
    }
    const FormatMemoryDomain* output_format = find_format(
        output_formats_, config.output_format.pixel_format(),
        config.output_format.memory_domain());
    if (output_format == NULL) {
        return mismatch(
            "video processor output pixel format or memory domain is unsupported");
    }
    if (!input_format->valid() || !output_format->valid()) {
        return mismatch("video processor capability has invalid plane layout constraints");
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
    if (request.require_zero_copy() && requires_explicit_conversion(request)) {
        return mismatch(
            "required zero-copy video processor plane layout or address alignment is unsupported");
    }
    return Status::ok_status();
}

bool VideoProcessorCapability::requires_explicit_conversion(
    const VideoProcessorRequest& request) const {
    const VideoProcessorConfig& config = request.config();
    const FormatMemoryDomain* input_format = find_format(
        input_formats_, config.input_format.pixel_format(),
        config.input_format.memory_domain());
    const FormatMemoryDomain* output_format = find_format(
        output_formats_, config.output_format.pixel_format(),
        config.output_format.memory_domain());
    return input_format == NULL || output_format == NULL ||
           !layout_matches(config.input_format, *input_format, input_width_,
                           input_height_, request.input_address_alignment()) ||
           !layout_matches(config.output_format, *output_format, output_width_,
                           output_height_, request.output_address_alignment());
}

bool VideoProcessorCapability::uses_zero_copy(
    const VideoProcessorRequest& request) const {
    return zero_copy_ && !requires_explicit_conversion(request);
}

Result<VideoProcessorConfig> VideoProcessorCapability::negotiated_config(
    const VideoProcessorRequest& request) const {
    const Status status = match(request);
    if (!status.ok()) {
        return Result<VideoProcessorConfig>(status);
    }
    const VideoProcessorConfig& config = request.config();
    if (!requires_explicit_conversion(request)) {
        return Result<VideoProcessorConfig>(config);
    }
    const FormatMemoryDomain* input_format = find_format(
        input_formats_, config.input_format.pixel_format(),
        config.input_format.memory_domain());
    const FormatMemoryDomain* output_format = find_format(
        output_formats_, config.output_format.pixel_format(),
        config.output_format.memory_domain());
    Result<VideoFormat> actual_input = make_actual_format(
        config.input_format, *input_format, input_width_, input_height_);
    if (!actual_input.ok()) {
        return Result<VideoProcessorConfig>(actual_input.status());
    }
    Result<VideoFormat> actual_output = make_actual_format(
        config.output_format, *output_format, output_width_, output_height_);
    if (!actual_output.ok()) {
        return Result<VideoProcessorConfig>(actual_output.status());
    }
    return VideoProcessorConfig::create(
        actual_input.value(), actual_output.value(), config.crop_x, config.crop_y,
        config.crop_width, config.crop_height, config.rotation_degrees);
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
    const FormatMemoryDomain* input_format = find_format(
        input_formats_, request.input_format().pixel_format(),
        request.input_format().memory_domain());
    if (input_format == NULL) {
        return mismatch(
            "video encoder input pixel format or memory domain is unsupported");
    }
    if (!input_format->valid()) {
        return mismatch("video encoder capability has invalid plane layout constraints");
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
    if (request.require_zero_copy() && requires_explicit_conversion(request)) {
        return mismatch(
            "required zero-copy video encoder plane layout or address alignment is unsupported");
    }
    return Status::ok_status();
}

bool VideoEncoderCapability::requires_explicit_conversion(
    const VideoEncoderRequest& request) const {
    const FormatMemoryDomain* input_format = find_format(
        input_formats_, request.input_format().pixel_format(),
        request.input_format().memory_domain());
    return input_format == NULL ||
           !layout_matches(request.input_format(), *input_format, width_, height_,
                           request.input_address_alignment());
}

bool VideoEncoderCapability::uses_zero_copy(
    const VideoEncoderRequest& request) const {
    return zero_copy_ && !requires_explicit_conversion(request);
}

Result<VideoFormat> VideoEncoderCapability::actual_input_format(
    const VideoEncoderRequest& request) const {
    const Status status = match(request);
    if (!status.ok()) {
        return Result<VideoFormat>(status);
    }
    if (!requires_explicit_conversion(request)) {
        return Result<VideoFormat>(request.input_format());
    }
    const FormatMemoryDomain* input_format = find_format(
        input_formats_, request.input_format().pixel_format(),
        request.input_format().memory_domain());
    return make_actual_format(request.input_format(), *input_format, width_,
                              height_);
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
    const Status provider_status =
        required_provider_status(provider_id_, request.constraints());
    if (!provider_status.ok()) {
        return provider_status;
    }
    if (!availability_status_.ok()) {
        return availability_status_;
    }
    if (processor_capabilities_.empty()) {
        return mismatch("provider has no video processor capability");
    }
    const Status first_mismatch = processor_capabilities_[0].match(request);
    if (first_mismatch.ok()) {
        return first_mismatch;
    }
    for (std::size_t index = 1; index < processor_capabilities_.size(); ++index) {
        const Status status = processor_capabilities_[index].match(request);
        if (status.ok()) {
            return status;
        }
    }
    return first_mismatch;
}

Status ProviderCapability::match(const VideoEncoderRequest& request) const {
    const Status provider_status =
        required_provider_status(provider_id_, request.constraints());
    if (!provider_status.ok()) {
        return provider_status;
    }
    if (!availability_status_.ok()) {
        return availability_status_;
    }
    if (encoder_capabilities_.empty()) {
        return mismatch("provider has no video encoder capability");
    }
    const Status first_mismatch = encoder_capabilities_[0].match(request);
    if (first_mismatch.ok()) {
        return first_mismatch;
    }
    for (std::size_t index = 1; index < encoder_capabilities_.size(); ++index) {
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
    for (std::size_t index = 0; index < processor_capabilities_.size(); ++index) {
        if (!processor_capabilities_[index].match(request).ok()) {
            continue;
        }
        Result<VideoProcessorConfig> config =
            processor_capabilities_[index].negotiated_config(request);
        if (!config.ok()) {
            return Result<VideoProcessorNegotiation>(config.status());
        }
        const bool conversion =
            processor_capabilities_[index].requires_explicit_conversion(request);
        const bool zero_copy =
            processor_capabilities_[index].uses_zero_copy(request);
        try {
            return Result<VideoProcessorNegotiation>(VideoProcessorNegotiation(
                provider_id_, config.value(), config.value().output_format,
                conversion, zero_copy));
        } catch (const std::bad_alloc&) {
            return Result<VideoProcessorNegotiation>(Status(
                StatusCode::kResourceExhausted,
                "failed to create video processor negotiation result"));
        }
    }
    return Result<VideoProcessorNegotiation>(status);
}

Result<VideoEncoderNegotiation> ProviderCapability::negotiate(
    const VideoEncoderRequest& request) const {
    const Status status = match(request);
    if (!status.ok()) {
        return Result<VideoEncoderNegotiation>(status);
    }
    for (std::size_t index = 0; index < encoder_capabilities_.size(); ++index) {
        if (!encoder_capabilities_[index].match(request).ok()) {
            continue;
        }
        Result<VideoFormat> actual_format =
            encoder_capabilities_[index].actual_input_format(request);
        if (!actual_format.ok()) {
            return Result<VideoEncoderNegotiation>(actual_format.status());
        }
        const bool conversion =
            encoder_capabilities_[index].requires_explicit_conversion(request);
        const bool zero_copy = encoder_capabilities_[index].uses_zero_copy(request);
        try {
            return Result<VideoEncoderNegotiation>(VideoEncoderNegotiation(
                provider_id_, request.config(), actual_format.value(), conversion,
                zero_copy));
        } catch (const std::bad_alloc&) {
            return Result<VideoEncoderNegotiation>(Status(
                StatusCode::kResourceExhausted,
                "failed to create video encoder negotiation result"));
        }
    }
    return Result<VideoEncoderNegotiation>(status);
}

Result<ProviderPreferenceScore> ProviderCapability::preference_score(
    const VideoProcessorRequest& request) const {
    const Status status = match(request);
    if (!status.ok()) {
        return Result<ProviderPreferenceScore>(status);
    }
    for (std::size_t index = 0; index < processor_capabilities_.size(); ++index) {
        if (processor_capabilities_[index].match(request).ok()) {
            try {
                return Result<ProviderPreferenceScore>(ProviderPreferenceScore(
                    preferred_provider_rank(
                        request.preferences().preferred_provider_ids(),
                        provider_id_),
                    provider_kind_rank(kind_,
                                       request.preferences().prefer_hardware()),
                    request.preferences().prefer_zero_copy() &&
                            !processor_capabilities_[index].uses_zero_copy(request)
                        ? 1
                        : 0,
                    provider_id_));
            } catch (const std::bad_alloc&) {
                return Result<ProviderPreferenceScore>(Status(
                    StatusCode::kResourceExhausted,
                    "failed to create provider preference score"));
            }
        }
    }
    return Result<ProviderPreferenceScore>(status);
}

Result<ProviderPreferenceScore> ProviderCapability::preference_score(
    const VideoEncoderRequest& request) const {
    const Status status = match(request);
    if (!status.ok()) {
        return Result<ProviderPreferenceScore>(status);
    }
    for (std::size_t index = 0; index < encoder_capabilities_.size(); ++index) {
        if (encoder_capabilities_[index].match(request).ok()) {
            try {
                return Result<ProviderPreferenceScore>(ProviderPreferenceScore(
                    preferred_provider_rank(
                        request.preferences().preferred_provider_ids(),
                        provider_id_),
                    provider_kind_rank(kind_,
                                       request.preferences().prefer_hardware()),
                    request.preferences().prefer_zero_copy() &&
                            !encoder_capabilities_[index].uses_zero_copy(request)
                        ? 1
                        : 0,
                    provider_id_));
            } catch (const std::bad_alloc&) {
                return Result<ProviderPreferenceScore>(Status(
                    StatusCode::kResourceExhausted,
                    "failed to create provider preference score"));
            }
        }
    }
    return Result<ProviderPreferenceScore>(status);
}

}  // namespace eavp
