#ifndef EAVP_TESTS_SUPPORT_AUDIO_TEST_UTILS_HPP_
#define EAVP_TESTS_SUPPORT_AUDIO_TEST_UTILS_HPP_

#include <cstddef>

#include "eavp/platform/linux/alsa_capture.hpp"

namespace eavp_test {

inline eavp::AlsaCaptureConfig make_alsa_config(
    eavp::SampleFormat sample_format =
        eavp::SampleFormat::kSigned16LittleEndian) {
    const eavp::Result<eavp::AudioFormat> format = eavp::AudioFormat::create(
        sample_format, 48000, eavp::AudioChannelLayout::kStereo,
        eavp::AudioSampleLayout::kInterleaved, eavp::MemoryDomain::kCpu);
    const eavp::Result<eavp::AlsaCaptureConfig> config =
        eavp::AlsaCaptureConfig::create("hw:Fake,0", format.value(), 480,
                                        480, 4);
    return config.value();
}

inline std::size_t pcm_bytes(std::size_t frames, std::size_t channels,
                             std::size_t bytes_per_sample) {
    return frames * channels * bytes_per_sample;
}

}  // namespace eavp_test

#endif  // EAVP_TESTS_SUPPORT_AUDIO_TEST_UTILS_HPP_
