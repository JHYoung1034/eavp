#ifndef EAVP_MEDIA_FRAME_HPP_
#define EAVP_MEDIA_FRAME_HPP_

#include <cstdint>
#include <new>

#include "eavp/base/time.hpp"
#include "eavp/media/audio_format.hpp"
#include "eavp/media/buffer.hpp"
#include "eavp/media/video_format.hpp"

namespace eavp {

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
        try {
            return Result<VideoFrame>(VideoFrame(buffer, format, pts, time_base));
        } catch (const std::bad_alloc&) {
            return Result<VideoFrame>(
                Status(StatusCode::kResourceExhausted, "failed to create video frame metadata"));
        }
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
    static Result<AudioFrame> create(const Buffer& buffer, const AudioFormat& format,
                                     int samples_per_channel, std::int64_t pts,
                                     const TimeBase& time_base, bool discontinuity);

    const Buffer& buffer() const { return buffer_; }
    const AudioFormat& format() const { return format_; }
    int samples_per_channel() const { return samples_per_channel_; }
    std::int64_t pts() const { return pts_; }
    const TimeBase& time_base() const { return time_base_; }
    bool discontinuity() const { return discontinuity_; }

private:
    AudioFrame(const Buffer& buffer, const AudioFormat& format, int samples_per_channel,
               std::int64_t pts, const TimeBase& time_base, bool discontinuity)
        : buffer_(buffer),
          format_(format),
          samples_per_channel_(samples_per_channel),
          pts_(pts),
          time_base_(time_base),
          discontinuity_(discontinuity) {}

    Buffer buffer_;
    AudioFormat format_;
    int samples_per_channel_;
    std::int64_t pts_;
    TimeBase time_base_;
    bool discontinuity_;
};

}  // namespace eavp

#endif  // EAVP_MEDIA_FRAME_HPP_
