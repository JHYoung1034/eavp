#ifndef EAVP_MEDIA_FRAME_HPP_
#define EAVP_MEDIA_FRAME_HPP_

#include <cstdint>

#include "eavp/base/time.hpp"
#include "eavp/media/buffer.hpp"
#include "eavp/media/video_format.hpp"

namespace eavp {

enum class SampleFormat {
    kUnknown,
    kSigned16,
    kSigned32,
    kFloat32,
};

class VideoFrame {
public:
    static Result<VideoFrame> create(const Buffer& buffer, const VideoFormat& format,
                                     std::int64_t pts,
                                     const TimeBase& time_base) {
        if (buffer.memory_domain() != format.memory_domain()) {
            return Result<VideoFrame>(
                Status(StatusCode::kCapabilityMismatch,
                       "video frame buffer memory domain does not match its format"));
        }
        if (buffer.plane_count() != format.planes().size()) {
            return Result<VideoFrame>(
                Status(StatusCode::kCapabilityMismatch,
                       "video frame buffer plane count does not match its format"));
        }
        for (std::size_t index = 0U; index < buffer.plane_count(); ++index) {
            const Result<PlaneLayout> buffer_layout = buffer.plane_layout(index);
            const PlaneLayout& format_layout = format.planes()[index];
            if (!buffer_layout.ok() || buffer_layout.value().offset != format_layout.offset ||
                buffer_layout.value().size != format_layout.size ||
                buffer_layout.value().stride != format_layout.stride) {
                return Result<VideoFrame>(
                    Status(StatusCode::kCapabilityMismatch,
                           "video frame buffer planes do not match its format"));
            }
        }
        return Result<VideoFrame>(VideoFrame(buffer, format, pts, time_base));
    }

    const Buffer& buffer() const { return buffer_; }
    const VideoFormat& format() const { return format_; }
    std::int64_t pts() const { return pts_; }
    const TimeBase& time_base() const { return time_base_; }

private:
    VideoFrame(const Buffer& buffer, const VideoFormat& format, std::int64_t pts,
               const TimeBase& time_base)
        : buffer_(buffer),
          format_(format),
          pts_(pts),
          time_base_(time_base) {}

    Buffer buffer_;
    VideoFormat format_;
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
