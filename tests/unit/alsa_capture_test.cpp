#include <gtest/gtest.h>

#include <limits>

#include "eavp/media/audio_format.hpp"
#include "eavp/platform/linux/alsa_capture.hpp"

namespace {

eavp::AudioFormat make_test_audio_format() {
    return eavp::AudioFormat::create(
        eavp::SampleFormat::kSigned16LittleEndian, 48000,
        eavp::AudioChannelLayout::kStereo,
        eavp::AudioSampleLayout::kInterleaved,
        eavp::MemoryDomain::kCpu).take_value();
}

TEST(AlsaCaptureConfigTest, AcceptsTheApprovedTenMillisecondShape) {
    const eavp::AudioFormat format = eavp::AudioFormat::create(
        eavp::SampleFormat::kSigned16LittleEndian, 48000,
        eavp::AudioChannelLayout::kStereo,
        eavp::AudioSampleLayout::kInterleaved,
        eavp::MemoryDomain::kCpu).take_value();
    const eavp::AlsaCaptureConfig config =
        eavp::AlsaCaptureConfig::create(
            "hw:Loopback,1,0", format, 480, 480, 4).take_value();

    EXPECT_EQ("hw:Loopback,1,0", config.device_name());
    EXPECT_EQ(480, config.samples_per_frame());
    EXPECT_EQ(480, config.period_size_hint());
    EXPECT_EQ(4, config.buffer_periods());
}

TEST(AlsaCaptureConfigTest, RejectsUnsupportedOrUnsafeShapes) {
    const eavp::AudioFormat cpu_format = make_test_audio_format();
    const eavp::AudioFormat mmap_format = eavp::AudioFormat::create(
        eavp::SampleFormat::kSigned16LittleEndian, 48000,
        eavp::AudioChannelLayout::kStereo,
        eavp::AudioSampleLayout::kInterleaved,
        eavp::MemoryDomain::kMmap).take_value();
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::AlsaCaptureConfig::create(
                  "", cpu_format, 480, 480, 4).status().code());
    EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch,
              eavp::AlsaCaptureConfig::create(
                  "hw:Loopback,1,0", mmap_format, 480, 480, 4)
                  .status().code());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::AlsaCaptureConfig::create(
                  "hw:Loopback,1,0", cpu_format, 0, 480, 4)
                  .status().code());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::AlsaCaptureConfig::create(
                  "hw:Loopback,1,0", cpu_format, 480, 0, 4)
                  .status().code());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::AlsaCaptureConfig::create(
                  "hw:Loopback,1,0", cpu_format, 480, 480, 0)
                  .status().code());
}

TEST(AlsaCaptureConfigTest, RejectsFrameByteCountThatOverflowsSizeType) {
    const eavp::AudioFormat format = make_test_audio_format();
    const int samples_per_frame = std::numeric_limits<int>::max();

    if (static_cast<std::size_t>(samples_per_frame) <=
        std::numeric_limits<std::size_t>::max() / format.bytes_per_pcm_frame()) {
        GTEST_SKIP() << "int-sized frame count cannot overflow size_t on this target";
    }

    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::AlsaCaptureConfig::create(
                  "hw:Loopback,1,0", format, samples_per_frame, 480, 4)
                  .status().code());
}

}  // namespace
