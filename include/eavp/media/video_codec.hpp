#ifndef EAVP_MEDIA_VIDEO_CODEC_HPP_
#define EAVP_MEDIA_VIDEO_CODEC_HPP_

#include <cstdint>
#include <vector>

#include "eavp/base/time.hpp"
#include "eavp/media/video_format.hpp"

namespace eavp {

enum class CodecId {
    kUnknown,
    kH264,
    kH265,
    kAac,
    kReference,
};

enum class RateControlMode { kCbr, kVbr, kConstantQuality };
enum class EncodedStreamFormat { kUnknown, kAnnexB, kAvcc, kHvcc, kReference };
enum class CodecProfile {
    kUnknown,
    kH264Baseline,
    kH264Main,
    kH264High,
    kH265Main,
};

struct VideoProcessorConfig {
    VideoFormat input_format;
    VideoFormat output_format;
    int crop_x;
    int crop_y;
    int crop_width;
    int crop_height;
    int rotation_degrees;
};

struct VideoEncoderConfig {
public:
    static Result<VideoEncoderConfig> create(
        CodecId codec_value, int width_value, int height_value, int frame_rate_numerator_value,
        int frame_rate_denominator_value, const TimeBase& time_base_value,
        std::int64_t target_bitrate_value, std::int64_t max_bitrate_value, int gop_length_value,
        int b_frames_value, RateControlMode rate_control_value, CodecProfile profile_value,
        int level_idc_value, bool low_latency_value);

    CodecId codec;
    int width;
    int height;
    int frame_rate_numerator;
    int frame_rate_denominator;
    TimeBase time_base;
    std::int64_t target_bitrate;
    std::int64_t max_bitrate;
    int gop_length;
    int b_frames;
    RateControlMode rate_control;
    CodecProfile profile;
    int level_idc;
    bool low_latency;

private:
    VideoEncoderConfig(CodecId codec_value, int width_value, int height_value,
                       int frame_rate_numerator_value, int frame_rate_denominator_value,
                       const TimeBase& time_base_value, std::int64_t target_bitrate_value,
                       std::int64_t max_bitrate_value, int gop_length_value, int b_frames_value,
                       RateControlMode rate_control_value, CodecProfile profile_value,
                       int level_idc_value, bool low_latency_value)
        : codec(codec_value),
          width(width_value),
          height(height_value),
          frame_rate_numerator(frame_rate_numerator_value),
          frame_rate_denominator(frame_rate_denominator_value),
          time_base(time_base_value),
          target_bitrate(target_bitrate_value),
          max_bitrate(max_bitrate_value),
          gop_length(gop_length_value),
          b_frames(b_frames_value),
          rate_control(rate_control_value),
          profile(profile_value),
          level_idc(level_idc_value),
          low_latency(low_latency_value) {}
};

struct CodecConfigData {
    std::vector<std::uint8_t> bytes;
};

}  // namespace eavp

#endif  // EAVP_MEDIA_VIDEO_CODEC_HPP_
