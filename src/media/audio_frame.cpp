#include "eavp/media/frame.hpp"

#include <limits>
#include <new>

namespace eavp {

Result<AudioFrame> AudioFrame::create(const Buffer& buffer, const AudioFormat& format,
                                      int samples_per_channel, std::int64_t pts,
                                      const TimeBase& time_base, bool discontinuity) {
    if (samples_per_channel <= 0 || time_base.numerator() <= 0) {
        return Result<AudioFrame>(Status(
            StatusCode::kInvalidArgument,
            "audio frame samples and time base must be positive"));
    }
    if (buffer.memory_domain() != format.memory_domain() || buffer.plane_count() != 1U) {
        return Result<AudioFrame>(Status(
            StatusCode::kCapabilityMismatch,
            "audio frame buffer does not match its format"));
    }
    const std::size_t pcm_frames = static_cast<std::size_t>(samples_per_channel);
    if (format.bytes_per_pcm_frame() >
        std::numeric_limits<std::size_t>::max() / pcm_frames) {
        return Result<AudioFrame>(Status(StatusCode::kInvalidArgument,
                                         "audio frame size overflows"));
    }
    const Result<PlaneLayout> plane = buffer.plane_layout(0U);
    if (!plane.ok() ||
        plane.value().size != pcm_frames * format.bytes_per_pcm_frame()) {
        return Result<AudioFrame>(Status(
            StatusCode::kCapabilityMismatch,
            "audio frame payload size does not match its format"));
    }
    try {
        return Result<AudioFrame>(AudioFrame(
            buffer, format, samples_per_channel, pts, time_base, discontinuity));
    } catch (const std::bad_alloc&) {
        return Result<AudioFrame>(Status(StatusCode::kResourceExhausted));
    } catch (...) {
        return Result<AudioFrame>(Status(StatusCode::kInternal));
    }
}

}  // namespace eavp
