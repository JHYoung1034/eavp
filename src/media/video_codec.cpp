#include "eavp/media/video_codec.hpp"

namespace eavp {

namespace {

bool is_video_codec(CodecId codec) {
    return codec == CodecId::kH264 || codec == CodecId::kH265 || codec == CodecId::kReference;
}

bool is_rate_control_mode(RateControlMode rate_control) {
    return rate_control == RateControlMode::kCbr || rate_control == RateControlMode::kVbr ||
           rate_control == RateControlMode::kConstantQuality;
}

}  // namespace

Result<VideoEncoderConfig> VideoEncoderConfig::create(
    CodecId codec, int width, int height, int frame_rate_numerator, int frame_rate_denominator,
    const TimeBase& time_base, std::int64_t target_bitrate, std::int64_t max_bitrate,
    int gop_length, int b_frames, RateControlMode rate_control, CodecProfile profile,
    int level_idc, bool low_latency) {
    if (!is_video_codec(codec)) {
        return Result<VideoEncoderConfig>(
            Status(StatusCode::kInvalidArgument, "video encoder codec must be supported"));
    }
    if (width <= 0 || height <= 0 || frame_rate_numerator <= 0 ||
        frame_rate_denominator <= 0 || time_base.numerator() <= 0 ||
        time_base.denominator() <= 0) {
        return Result<VideoEncoderConfig>(
            Status(StatusCode::kInvalidArgument, "video encoder dimensions and timing must be positive"));
    }
    if (target_bitrate <= 0 || max_bitrate < target_bitrate || gop_length <= 0 || b_frames < 0 ||
        level_idc < 0 || !is_rate_control_mode(rate_control)) {
        return Result<VideoEncoderConfig>(
            Status(StatusCode::kInvalidArgument, "video encoder configuration is invalid"));
    }
    return Result<VideoEncoderConfig>(
        VideoEncoderConfig(codec, width, height, frame_rate_numerator, frame_rate_denominator,
                           time_base, target_bitrate, max_bitrate, gop_length, b_frames,
                           rate_control, profile, level_idc, low_latency));
}

}  // namespace eavp
