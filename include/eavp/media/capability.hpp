#ifndef EAVP_MEDIA_CAPABILITY_HPP_
#define EAVP_MEDIA_CAPABILITY_HPP_

#include <string>
#include <vector>

#include "eavp/base/result.hpp"
#include "eavp/media/video_codec.hpp"

namespace eavp {

enum class ProviderKind { kReference, kSoftware, kHardware };

enum class VideoProcessingOperation {
    kScaling,
    kCropping,
    kRotation,
    kFormatConversion,
};

class DimensionRange {
public:
    DimensionRange(int minimum, int maximum, int step, int alignment)
        : minimum_(minimum),
          maximum_(maximum),
          step_(step),
          alignment_(alignment) {}

    bool valid() const;
    bool contains(int value) const;

    int minimum() const { return minimum_; }
    int maximum() const { return maximum_; }
    int step() const { return step_; }
    int alignment() const { return alignment_; }

private:
    int minimum_;
    int maximum_;
    int step_;
    int alignment_;
};

class FormatMemoryDomain {
public:
    FormatMemoryDomain(PixelFormat pixel_format, MemoryDomain memory_domain)
        : pixel_format_(pixel_format), memory_domain_(memory_domain) {}

    PixelFormat pixel_format() const { return pixel_format_; }
    MemoryDomain memory_domain() const { return memory_domain_; }

private:
    PixelFormat pixel_format_;
    MemoryDomain memory_domain_;
};

class SelectionPreferences {
public:
    SelectionPreferences(const std::vector<std::string>& preferred_provider_ids,
                         bool prefer_hardware, bool prefer_zero_copy)
        : preferred_provider_ids_(preferred_provider_ids),
          prefer_hardware_(prefer_hardware),
          prefer_zero_copy_(prefer_zero_copy) {}

    const std::vector<std::string>& preferred_provider_ids() const {
        return preferred_provider_ids_;
    }
    bool prefer_hardware() const { return prefer_hardware_; }
    bool prefer_zero_copy() const { return prefer_zero_copy_; }

private:
    std::vector<std::string> preferred_provider_ids_;
    bool prefer_hardware_;
    bool prefer_zero_copy_;
};

class VideoProcessorRequest {
public:
    VideoProcessorRequest(
        const VideoProcessorConfig& config,
        const std::vector<VideoProcessingOperation>& required_operations,
        bool require_zero_copy, const SelectionPreferences& preferences)
        : config_(config),
          required_operations_(required_operations),
          require_zero_copy_(require_zero_copy),
          preferences_(preferences) {}

    const VideoProcessorConfig& config() const { return config_; }
    const std::vector<VideoProcessingOperation>& required_operations() const {
        return required_operations_;
    }
    bool require_zero_copy() const { return require_zero_copy_; }
    const SelectionPreferences& preferences() const { return preferences_; }

private:
    VideoProcessorConfig config_;
    std::vector<VideoProcessingOperation> required_operations_;
    bool require_zero_copy_;
    SelectionPreferences preferences_;
};

class VideoEncoderRequest {
public:
    VideoEncoderRequest(const VideoFormat& input_format,
                        const VideoEncoderConfig& config,
                        bool require_zero_copy,
                        const SelectionPreferences& preferences)
        : input_format_(input_format),
          config_(config),
          require_zero_copy_(require_zero_copy),
          preferences_(preferences) {}

    const VideoFormat& input_format() const { return input_format_; }
    const VideoEncoderConfig& config() const { return config_; }
    bool require_zero_copy() const { return require_zero_copy_; }
    const SelectionPreferences& preferences() const { return preferences_; }

private:
    VideoFormat input_format_;
    VideoEncoderConfig config_;
    bool require_zero_copy_;
    SelectionPreferences preferences_;
};

class VideoProcessorCapability {
public:
    VideoProcessorCapability(
        const DimensionRange& input_width, const DimensionRange& input_height,
        const DimensionRange& output_width, const DimensionRange& output_height,
        const std::vector<FormatMemoryDomain>& input_formats,
        const std::vector<FormatMemoryDomain>& output_formats,
        const std::vector<VideoProcessingOperation>& operations,
        bool zero_copy)
        : input_width_(input_width),
          input_height_(input_height),
          output_width_(output_width),
          output_height_(output_height),
          input_formats_(input_formats),
          output_formats_(output_formats),
          operations_(operations),
          zero_copy_(zero_copy) {}

    Status match(const VideoProcessorRequest& request) const;
    bool supports(const VideoProcessorRequest& request) const {
        return match(request).ok();
    }

    const DimensionRange& input_width() const { return input_width_; }
    const DimensionRange& input_height() const { return input_height_; }
    const DimensionRange& output_width() const { return output_width_; }
    const DimensionRange& output_height() const { return output_height_; }
    const std::vector<FormatMemoryDomain>& input_formats() const {
        return input_formats_;
    }
    const std::vector<FormatMemoryDomain>& output_formats() const {
        return output_formats_;
    }
    const std::vector<VideoProcessingOperation>& operations() const {
        return operations_;
    }
    bool zero_copy() const { return zero_copy_; }

private:
    DimensionRange input_width_;
    DimensionRange input_height_;
    DimensionRange output_width_;
    DimensionRange output_height_;
    std::vector<FormatMemoryDomain> input_formats_;
    std::vector<FormatMemoryDomain> output_formats_;
    std::vector<VideoProcessingOperation> operations_;
    bool zero_copy_;
};

class VideoEncoderCapability {
public:
    VideoEncoderCapability(
        const DimensionRange& width, const DimensionRange& height, CodecId codec,
        const std::vector<CodecProfile>& profiles,
        const std::vector<int>& levels,
        const std::vector<FormatMemoryDomain>& input_formats,
        const std::vector<RateControlMode>& rate_control_modes,
        bool zero_copy)
        : width_(width),
          height_(height),
          codec_(codec),
          profiles_(profiles),
          levels_(levels),
          input_formats_(input_formats),
          rate_control_modes_(rate_control_modes),
          zero_copy_(zero_copy) {}

    Status match(const VideoEncoderRequest& request) const;
    bool supports(const VideoEncoderRequest& request) const {
        return match(request).ok();
    }

    const DimensionRange& width() const { return width_; }
    const DimensionRange& height() const { return height_; }
    CodecId codec() const { return codec_; }
    const std::vector<CodecProfile>& profiles() const { return profiles_; }
    const std::vector<int>& levels() const { return levels_; }
    const std::vector<FormatMemoryDomain>& input_formats() const {
        return input_formats_;
    }
    const std::vector<RateControlMode>& rate_control_modes() const {
        return rate_control_modes_;
    }
    bool zero_copy() const { return zero_copy_; }

private:
    DimensionRange width_;
    DimensionRange height_;
    CodecId codec_;
    std::vector<CodecProfile> profiles_;
    std::vector<int> levels_;
    std::vector<FormatMemoryDomain> input_formats_;
    std::vector<RateControlMode> rate_control_modes_;
    bool zero_copy_;
};

struct VideoProcessorNegotiation {
    VideoProcessorNegotiation(const std::string& provider_id_value,
                              const VideoProcessorConfig& config_value,
                              const VideoFormat& actual_format_value,
                              bool requires_explicit_conversion_value)
        : provider_id(provider_id_value),
          config(config_value),
          actual_format(actual_format_value),
          requires_explicit_conversion(requires_explicit_conversion_value) {}

    std::string provider_id;
    VideoProcessorConfig config;
    VideoFormat actual_format;
    bool requires_explicit_conversion;
};

struct VideoEncoderNegotiation {
    VideoEncoderNegotiation(const std::string& provider_id_value,
                            const VideoEncoderConfig& config_value,
                            const VideoFormat& actual_format_value,
                            bool requires_explicit_conversion_value)
        : provider_id(provider_id_value),
          config(config_value),
          actual_format(actual_format_value),
          requires_explicit_conversion(requires_explicit_conversion_value) {}

    std::string provider_id;
    VideoEncoderConfig config;
    VideoFormat actual_format;
    bool requires_explicit_conversion;
};

class ProviderPreferenceScore {
public:
    ProviderPreferenceScore(int provider_rank, int kind_rank,
                            int zero_copy_rank,
                            const std::string& provider_id)
        : provider_rank_(provider_rank),
          kind_rank_(kind_rank),
          zero_copy_rank_(zero_copy_rank),
          provider_id_(provider_id) {}

    bool operator<(const ProviderPreferenceScore& other) const;

    int provider_rank() const { return provider_rank_; }
    int kind_rank() const { return kind_rank_; }
    int zero_copy_rank() const { return zero_copy_rank_; }
    const std::string& provider_id() const { return provider_id_; }

private:
    int provider_rank_;
    int kind_rank_;
    int zero_copy_rank_;
    std::string provider_id_;
};

class ProviderCapability {
public:
    ProviderCapability(
        const std::string& provider_id,
        const std::string& implementation_version,
        const std::string& device_id, const Status& availability_status,
        ProviderKind kind,
        const std::vector<VideoProcessorCapability>& processor_capabilities,
        const std::vector<VideoEncoderCapability>& encoder_capabilities,
        int max_concurrent_instances,
        const std::vector<std::string>& resource_constraints)
        : provider_id_(provider_id),
          implementation_version_(implementation_version),
          device_id_(device_id),
          availability_status_(availability_status),
          kind_(kind),
          processor_capabilities_(processor_capabilities),
          encoder_capabilities_(encoder_capabilities),
          max_concurrent_instances_(max_concurrent_instances),
          resource_constraints_(resource_constraints) {}

    const std::string& provider_id() const { return provider_id_; }
    const std::string& implementation_version() const {
        return implementation_version_;
    }
    const std::string& device_id() const { return device_id_; }
    bool available() const { return availability_status_.ok(); }
    const Status& availability_status() const { return availability_status_; }
    ProviderKind kind() const { return kind_; }
    const std::vector<VideoProcessorCapability>& processor_capabilities() const {
        return processor_capabilities_;
    }
    const std::vector<VideoEncoderCapability>& encoder_capabilities() const {
        return encoder_capabilities_;
    }
    int max_concurrent_instances() const { return max_concurrent_instances_; }
    const std::vector<std::string>& resource_constraints() const {
        return resource_constraints_;
    }

    Status match(const VideoProcessorRequest& request) const;
    Status match(const VideoEncoderRequest& request) const;
    bool supports(const VideoProcessorRequest& request) const {
        return match(request).ok();
    }
    bool supports(const VideoEncoderRequest& request) const {
        return match(request).ok();
    }
    Result<VideoProcessorNegotiation> negotiate(
        const VideoProcessorRequest& request) const;
    Result<VideoEncoderNegotiation> negotiate(
        const VideoEncoderRequest& request) const;
    ProviderPreferenceScore preference_score(
        const SelectionPreferences& preferences, bool zero_copy) const;

private:
    std::string provider_id_;
    std::string implementation_version_;
    std::string device_id_;
    Status availability_status_;
    ProviderKind kind_;
    std::vector<VideoProcessorCapability> processor_capabilities_;
    std::vector<VideoEncoderCapability> encoder_capabilities_;
    int max_concurrent_instances_;
    std::vector<std::string> resource_constraints_;
};

}  // namespace eavp

#endif  // EAVP_MEDIA_CAPABILITY_HPP_
