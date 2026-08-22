#ifndef EAVP_MEDIA_AUDIO_FORMAT_HPP_
#define EAVP_MEDIA_AUDIO_FORMAT_HPP_

#include <cstddef>

#include "eavp/media/buffer.hpp"

namespace eavp {

enum class SampleFormat {
    kUnknown,
    kSigned16LittleEndian,
    kSigned24In32LittleEndian,
    kSigned32LittleEndian,
    kFloat32LittleEndian,
};

enum class AudioSampleLayout {
    kInterleaved,
};

enum class AudioChannelLayout {
    kMono,
    kStereo,
};

class AudioFormat {
public:
    static Result<AudioFormat> create(SampleFormat sample_format, int sample_rate,
                                      AudioChannelLayout channel_layout,
                                      AudioSampleLayout sample_layout,
                                      MemoryDomain memory_domain);

    SampleFormat sample_format() const { return sample_format_; }
    int sample_rate() const { return sample_rate_; }
    AudioChannelLayout channel_layout() const { return channel_layout_; }
    AudioSampleLayout sample_layout() const { return sample_layout_; }
    MemoryDomain memory_domain() const { return memory_domain_; }
    int channels() const { return channels_; }
    std::size_t bytes_per_sample() const { return bytes_per_sample_; }
    std::size_t bytes_per_pcm_frame() const { return bytes_per_pcm_frame_; }

private:
    AudioFormat(SampleFormat sample_format, int sample_rate,
                AudioChannelLayout channel_layout, AudioSampleLayout sample_layout,
                MemoryDomain memory_domain, int channels,
                std::size_t bytes_per_sample, std::size_t bytes_per_pcm_frame)
        : sample_format_(sample_format),
          sample_rate_(sample_rate),
          channel_layout_(channel_layout),
          sample_layout_(sample_layout),
          memory_domain_(memory_domain),
          channels_(channels),
          bytes_per_sample_(bytes_per_sample),
          bytes_per_pcm_frame_(bytes_per_pcm_frame) {}

    SampleFormat sample_format_;
    int sample_rate_;
    AudioChannelLayout channel_layout_;
    AudioSampleLayout sample_layout_;
    MemoryDomain memory_domain_;
    int channels_;
    std::size_t bytes_per_sample_;
    std::size_t bytes_per_pcm_frame_;
};

}  // namespace eavp

#endif  // EAVP_MEDIA_AUDIO_FORMAT_HPP_
