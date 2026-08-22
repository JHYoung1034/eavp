#include "eavp/media/audio_format.hpp"

#include <limits>
#include <new>

namespace eavp {

namespace {

std::size_t sample_bytes(SampleFormat format) {
    switch (format) {
    case SampleFormat::kSigned16LittleEndian:
        return 2U;
    case SampleFormat::kSigned24In32LittleEndian:
    case SampleFormat::kSigned32LittleEndian:
    case SampleFormat::kFloat32LittleEndian:
        return 4U;
    case SampleFormat::kUnknown:
        return 0U;
    }
    return 0U;
}

int channel_count(AudioChannelLayout layout) {
    switch (layout) {
    case AudioChannelLayout::kMono:
        return 1;
    case AudioChannelLayout::kStereo:
        return 2;
    }
    return 0;
}

bool is_memory_domain(MemoryDomain memory_domain) {
    switch (memory_domain) {
    case MemoryDomain::kCpu:
    case MemoryDomain::kMmap:
    case MemoryDomain::kDmaBuf:
    case MemoryDomain::kDeviceOpaque:
        return true;
    }
    return false;
}

}  // namespace

Result<AudioFormat> AudioFormat::create(SampleFormat sample_format, int sample_rate,
                                        AudioChannelLayout channel_layout,
                                        AudioSampleLayout sample_layout,
                                        MemoryDomain memory_domain) {
    try {
        const std::size_t bytes_per_sample = sample_bytes(sample_format);
        const int channels = channel_count(channel_layout);
        if (bytes_per_sample == 0U || sample_rate <= 0 || channels <= 0 ||
            sample_layout != AudioSampleLayout::kInterleaved ||
            !is_memory_domain(memory_domain)) {
            return Result<AudioFormat>(
                Status(StatusCode::kInvalidArgument, "audio format is invalid"));
        }
        const std::size_t channel_count_value = static_cast<std::size_t>(channels);
        if (bytes_per_sample >
            std::numeric_limits<std::size_t>::max() / channel_count_value) {
            return Result<AudioFormat>(
                Status(StatusCode::kInvalidArgument, "audio PCM frame size overflows"));
        }
        const std::size_t bytes_per_pcm_frame = bytes_per_sample * channel_count_value;
        return Result<AudioFormat>(AudioFormat(
            sample_format, sample_rate, channel_layout, sample_layout, memory_domain,
            channels, bytes_per_sample, bytes_per_pcm_frame));
    } catch (const std::bad_alloc&) {
        return Result<AudioFormat>(Status(StatusCode::kResourceExhausted));
    } catch (...) {
        return Result<AudioFormat>(Status(StatusCode::kInternal));
    }
}

}  // namespace eavp
