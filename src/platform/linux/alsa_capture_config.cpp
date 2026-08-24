#include "eavp/platform/linux/alsa_capture.hpp"

#include <limits>
#include <new>

namespace eavp {

Result<AlsaCaptureConfig> AlsaCaptureConfig::create(const std::string& device_name,
                                                     const AudioFormat& format,
                                                     int samples_per_frame,
                                                     int period_size_hint,
                                                     int buffer_periods) {
    try {
        if (device_name.empty() || samples_per_frame <= 0 ||
            period_size_hint <= 0 || buffer_periods <= 0) {
            return Result<AlsaCaptureConfig>(Status(
                StatusCode::kInvalidArgument,
                "ALSA capture configuration values must be positive"));
        }
        if (format.memory_domain() != MemoryDomain::kCpu ||
            format.sample_layout() != AudioSampleLayout::kInterleaved) {
            return Result<AlsaCaptureConfig>(Status(
                StatusCode::kCapabilityMismatch,
                "ALSA capture requires interleaved CPU audio"));
        }

        const std::size_t frames =
            static_cast<std::size_t>(samples_per_frame);
        if (format.bytes_per_pcm_frame() >
            std::numeric_limits<std::size_t>::max() / frames) {
            return Result<AlsaCaptureConfig>(Status(
                StatusCode::kInvalidArgument,
                "ALSA capture frame size overflows"));
        }

        return Result<AlsaCaptureConfig>(AlsaCaptureConfig(
            device_name, format, samples_per_frame, period_size_hint, buffer_periods));
    } catch (const std::bad_alloc&) {
        return Result<AlsaCaptureConfig>(Status(StatusCode::kResourceExhausted));
    } catch (...) {
        return Result<AlsaCaptureConfig>(Status(StatusCode::kInternal));
    }
}

}  // namespace eavp
