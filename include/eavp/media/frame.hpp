#ifndef EAVP_MEDIA_FRAME_HPP_
#define EAVP_MEDIA_FRAME_HPP_

#include <cstdint>

#include "eavp/base/time.hpp"
#include "eavp/media/buffer.hpp"

namespace eavp {

enum class PixelFormat {
    kUnknown,
    kNv12,
    kYuv420p,
    kRgb24,
};

enum class SampleFormat {
    kUnknown,
    kSigned16,
    kSigned32,
    kFloat32,
};

class VideoFrame {
public:
    static Result<VideoFrame> create(const Buffer& buffer, PixelFormat format, int width,
                                     int height, int stride, std::int64_t pts,
                                     const TimeBase& time_base) {
        if (width <= 0 || height <= 0 || stride <= 0) {
            return Result<VideoFrame>(
                Status(StatusCode::kInvalidArgument, "video frame shape must be positive"));
        }
        return Result<VideoFrame>(
            VideoFrame(buffer, format, width, height, stride, pts, time_base));
    }

    const Buffer& buffer() const { return buffer_; }
    PixelFormat format() const { return format_; }
    int width() const { return width_; }
    int height() const { return height_; }
    int stride() const { return stride_; }
    std::int64_t pts() const { return pts_; }
    const TimeBase& time_base() const { return time_base_; }

private:
    VideoFrame(const Buffer& buffer, PixelFormat format, int width, int height, int stride,
               std::int64_t pts, const TimeBase& time_base)
        : buffer_(buffer),
          format_(format),
          width_(width),
          height_(height),
          stride_(stride),
          pts_(pts),
          time_base_(time_base) {}

    Buffer buffer_;
    PixelFormat format_;
    int width_;
    int height_;
    int stride_;
    std::int64_t pts_;
    TimeBase time_base_;
};

class AudioFrame {
public:
    static Result<AudioFrame> create(const Buffer& buffer, SampleFormat format, int sample_rate,
                                     int channels, int samples, std::int64_t pts,
                                     const TimeBase& time_base) {
        if (sample_rate <= 0 || channels <= 0 || samples <= 0) {
            return Result<AudioFrame>(
                Status(StatusCode::kInvalidArgument, "audio frame shape must be positive"));
        }
        return Result<AudioFrame>(
            AudioFrame(buffer, format, sample_rate, channels, samples, pts, time_base));
    }

    const Buffer& buffer() const { return buffer_; }
    SampleFormat format() const { return format_; }
    int sample_rate() const { return sample_rate_; }
    int channels() const { return channels_; }
    int samples() const { return samples_; }
    std::int64_t pts() const { return pts_; }
    const TimeBase& time_base() const { return time_base_; }

private:
    AudioFrame(const Buffer& buffer, SampleFormat format, int sample_rate, int channels,
               int samples, std::int64_t pts, const TimeBase& time_base)
        : buffer_(buffer),
          format_(format),
          sample_rate_(sample_rate),
          channels_(channels),
          samples_(samples),
          pts_(pts),
          time_base_(time_base) {}

    Buffer buffer_;
    SampleFormat format_;
    int sample_rate_;
    int channels_;
    int samples_;
    std::int64_t pts_;
    TimeBase time_base_;
};

}  // namespace eavp

#endif  // EAVP_MEDIA_FRAME_HPP_

