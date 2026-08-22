#include <cerrno>
#include <limits>
#include <memory>

#include <gtest/gtest.h>

#include "platform/linux/alsa_system.hpp"
#include "support/audio_test_utils.hpp"
#include "support/fake_alsa_api.hpp"

namespace {

using eavp_test::FakeAlsaApi;
using eavp_test::make_alsa_config;

TEST(AlsaSystemTest, ConfiguresExactInterleavedCaptureAndReadsBackValues) {
    std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
    FakeAlsaApi* observed = fake.get();
    observed->negotiated_rate = 48000U;
    observed->negotiated_channels = 2U;
    observed->negotiated_period = 512U;
    observed->negotiated_buffer = 2048U;
    eavp::detail::AlsaSystem system(std::move(fake));

    ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
    EXPECT_EQ(SND_PCM_STREAM_CAPTURE, observed->opened_stream);
    EXPECT_EQ(SND_PCM_ACCESS_RW_INTERLEAVED, observed->requested_access);
    EXPECT_EQ(SND_PCM_FORMAT_S16_LE, observed->requested_format);
    EXPECT_EQ(48000U, observed->requested_rate);
    EXPECT_EQ(2U, observed->requested_channels);
    EXPECT_EQ(512U, system.negotiated().period_frames);
    EXPECT_EQ(2048U, system.negotiated().buffer_frames);
    EXPECT_TRUE(system.negotiated().monotonic_timestamp);
}

TEST(AlsaSystemTest, MapsEveryApprovedSampleFormatExactly) {
    struct Case {
        eavp::SampleFormat eavp_format;
        snd_pcm_format_t alsa_format;
    };
    const Case cases[] = {
        {eavp::SampleFormat::kSigned16LittleEndian, SND_PCM_FORMAT_S16_LE},
        {eavp::SampleFormat::kSigned24In32LittleEndian, SND_PCM_FORMAT_S24_LE},
        {eavp::SampleFormat::kSigned32LittleEndian, SND_PCM_FORMAT_S32_LE},
        {eavp::SampleFormat::kFloat32LittleEndian, SND_PCM_FORMAT_FLOAT_LE},
    };
    for (std::size_t index = 0U; index < 4U; ++index) {
        std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
        FakeAlsaApi* observed = fake.get();
        eavp::detail::AlsaSystem system(std::move(fake));
        ASSERT_TRUE(system.prepare(make_alsa_config(cases[index].eavp_format)).ok());
        EXPECT_EQ(cases[index].alsa_format, observed->requested_format);
    }
}

TEST(AlsaSystemTest, ClosesEveryPartiallyPreparedDevice) {
    for (int step = FakeAlsaApi::kOpen; step <= FakeAlsaApi::kPcmPrepare; ++step) {
        std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
        FakeAlsaApi* observed = fake.get();
        observed->fail_step = step;
        eavp::detail::AlsaSystem system(std::move(fake));
        EXPECT_FALSE(system.prepare(make_alsa_config()).ok()) << "step " << step;
        EXPECT_EQ(observed->successful_open_count, observed->close_count) << step;
        EXPECT_EQ(observed->hw_params_alloc_count, observed->hw_params_free_count)
            << step;
        EXPECT_EQ(observed->sw_params_alloc_count, observed->sw_params_free_count)
            << step;
    }
}

TEST(AlsaSystemTest, RejectsNegotiatedRateOrFormatChanges) {
    std::unique_ptr<FakeAlsaApi> rate_fake(new FakeAlsaApi());
    FakeAlsaApi* rate_observed = rate_fake.get();
    rate_observed->negotiated_rate = 44100U;
    eavp::detail::AlsaSystem rate_system(std::move(rate_fake));
    const eavp::Status rate_status = rate_system.prepare(make_alsa_config());
    EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch, rate_status.code());

    std::unique_ptr<FakeAlsaApi> format_fake(new FakeAlsaApi());
    FakeAlsaApi* format_observed = format_fake.get();
    format_observed->accepts_requested_format = false;
    format_observed->negotiated_format = SND_PCM_FORMAT_S32_LE;
    eavp::detail::AlsaSystem format_system(std::move(format_fake));
    const eavp::Status format_status = format_system.prepare(make_alsa_config());
    EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch, format_status.code());
    EXPECT_EQ(1, format_observed->close_count);
}

TEST(AlsaSystemTest, RejectsBufferSmallerThanTwoExtremePeriodsWithoutOverflow) {
    std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
    FakeAlsaApi* observed = fake.get();
    observed->reported_period = std::numeric_limits<snd_pcm_uframes_t>::max();
    observed->negotiated_buffer = std::numeric_limits<snd_pcm_uframes_t>::max();
    eavp::detail::AlsaSystem system(std::move(fake));

    const eavp::Status status = system.prepare(make_alsa_config());
    EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch, status.code());
    EXPECT_EQ(1, observed->close_count);
}

TEST(AlsaSystemTest, StartAndStopAreIdempotent) {
    std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
    FakeAlsaApi* observed = fake.get();
    eavp::detail::AlsaSystem system(std::move(fake));

    ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
    EXPECT_TRUE(system.start().ok());
    EXPECT_TRUE(system.start().ok());
    EXPECT_EQ(1, observed->pcm_start_count);
    EXPECT_TRUE(system.stop().ok());
    EXPECT_TRUE(system.stop().ok());
    EXPECT_EQ(1, observed->pcm_drop_count);
    EXPECT_EQ(1, observed->close_count);
}

TEST(AlsaSystemTest, StopClosesWhenDropFailsAndRetainsFirstError) {
    std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
    FakeAlsaApi* observed = fake.get();
    eavp::detail::AlsaSystem system(std::move(fake));
    ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
    observed->fail_step = FakeAlsaApi::kPcmDrop;
    observed->fail_code = -EPIPE;

    const eavp::Status status = system.stop();
    EXPECT_EQ(eavp::StatusCode::kIoError, status.code());
    EXPECT_EQ(-EPIPE, status.native_code());
    EXPECT_EQ(1, observed->close_count);
}

TEST(AlsaSystemTest, MapsMissingDeviceAndCapabilityMismatch) {
    std::unique_ptr<FakeAlsaApi> missing_fake(new FakeAlsaApi());
    FakeAlsaApi* missing_observed = missing_fake.get();
    missing_observed->fail_step = FakeAlsaApi::kOpen;
    missing_observed->fail_code = -ENOENT;
    eavp::detail::AlsaSystem missing_system(std::move(missing_fake));
    const eavp::Status missing = missing_system.prepare(make_alsa_config());
    EXPECT_EQ(eavp::StatusCode::kNotFound, missing.code());
    EXPECT_EQ("alsa", missing.provider_id());
    EXPECT_EQ("snd_pcm_open", missing.operation());
    EXPECT_EQ(-ENOENT, missing.native_code());

    std::unique_ptr<FakeAlsaApi> capability_fake(new FakeAlsaApi());
    capability_fake->fail_step = FakeAlsaApi::kHwParamsSetFormat;
    capability_fake->fail_code = -EINVAL;
    eavp::detail::AlsaSystem capability_system(std::move(capability_fake));
    const eavp::Status capability = capability_system.prepare(make_alsa_config());
    EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch, capability.code());
    EXPECT_EQ(-EINVAL, capability.native_code());
}

TEST(AlsaSystemTest, UnsupportedMonotonicTimestampEnablesFallbackWithoutUsingRealtime) {
    std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
    FakeAlsaApi* observed = fake.get();
    observed->fail_step = FakeAlsaApi::kSwParamsSetTstampType;
    observed->fail_code = -EINVAL;
    eavp::detail::AlsaSystem system(std::move(fake));

    ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
    EXPECT_FALSE(system.negotiated().monotonic_timestamp);
    EXPECT_EQ(0, observed->htimestamp_count);
    EXPECT_EQ(0, observed->monotonic_now_count);
}

TEST(AlsaSystemTest, TimestampConvertsAvailableFramesToFirstUnreadSamplePts) {
    std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
    FakeAlsaApi* observed = fake.get();
    observed->htimestamp_value.tv_sec = 2;
    observed->htimestamp_value.tv_nsec = 0L;
    observed->htimestamp_available = 480U;
    observed->monotonic_now_value.tv_sec = 2;
    eavp::detail::AlsaSystem system(std::move(fake));
    ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
    ASSERT_TRUE(system.start().ok());

    const eavp::Result<eavp::detail::AlsaAnchor> anchor = system.capture_anchor();

    ASSERT_TRUE(anchor.ok());
    EXPECT_EQ(1990000, anchor.value().first_unread_pts_us);
    EXPECT_FALSE(anchor.value().used_fallback);
    EXPECT_EQ(1, observed->htimestamp_count);
    EXPECT_EQ(1, observed->monotonic_now_count);
}

TEST(AlsaSystemTest, TimestampFallsBackToMonotonicClockWhenHtimestampFails) {
    std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
    FakeAlsaApi* observed = fake.get();
    observed->htimestamp_result = -EIO;
    observed->avail_update_result = 480;
    observed->monotonic_now_value.tv_sec = 3;
    eavp::detail::AlsaSystem system(std::move(fake));
    ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
    ASSERT_TRUE(system.start().ok());

    const eavp::Result<eavp::detail::AlsaAnchor> anchor = system.capture_anchor();

    ASSERT_TRUE(anchor.ok());
    EXPECT_EQ(2990000, anchor.value().first_unread_pts_us);
    EXPECT_TRUE(anchor.value().used_fallback);
    EXPECT_EQ(1, observed->avail_update_count);
}

TEST(AlsaSystemTest, TimestampRejectsInvalidOrClearlyFutureAnchorData) {
    std::unique_ptr<FakeAlsaApi> invalid_fake(new FakeAlsaApi());
    invalid_fake->htimestamp_value.tv_sec = 1;
    invalid_fake->htimestamp_value.tv_nsec = 1000000000L;
    invalid_fake->avail_update_result = -1;
    eavp::detail::AlsaSystem invalid_system(std::move(invalid_fake));
    ASSERT_TRUE(invalid_system.prepare(make_alsa_config()).ok());
    ASSERT_TRUE(invalid_system.start().ok());
    EXPECT_FALSE(invalid_system.capture_anchor().ok());

    std::unique_ptr<FakeAlsaApi> future_fake(new FakeAlsaApi());
    future_fake->htimestamp_value.tv_sec = 3;
    future_fake->monotonic_now_value.tv_sec = 1;
    future_fake->avail_update_result = -1;
    eavp::detail::AlsaSystem future_system(std::move(future_fake));
    ASSERT_TRUE(future_system.prepare(make_alsa_config()).ok());
    ASSERT_TRUE(future_system.start().ok());
    EXPECT_FALSE(future_system.capture_anchor().ok());
}

TEST(AlsaSystemTest, TimestampAcceptsOneSecondOfMonotonicSchedulingJitter) {
    std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
    fake->htimestamp_value.tv_sec = 2;
    fake->monotonic_now_value.tv_sec = 1;
    eavp::detail::AlsaSystem system(std::move(fake));
    ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
    ASSERT_TRUE(system.start().ok());

    EXPECT_TRUE(system.capture_anchor().ok());
}

TEST(AlsaSystemTest, TimestampRejectsOverflowWithoutGeneratingPts) {
    std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
    fake->htimestamp_value.tv_sec = std::numeric_limits<time_t>::max();
    fake->htimestamp_value.tv_nsec = 999999999L;
    fake->avail_update_result = -1;
    eavp::detail::AlsaSystem system(std::move(fake));
    ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
    ASSERT_TRUE(system.start().ok());

    EXPECT_FALSE(system.capture_anchor().ok());
}

TEST(AlsaSystemTest, XrunRecoversOnceAndReturnsTimelineDiscontinuity) {
    std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
    FakeAlsaApi* observed = fake.get();
    observed->pcm_read_results.push_back(-EPIPE);
    eavp::detail::AlsaSystem system(std::move(fake));
    ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
    ASSERT_TRUE(system.start().ok());

    const eavp::Result<eavp::detail::AlsaReadResult> read =
        system.read_interleaved(reinterpret_cast<std::uint8_t*>(observed), 480);

    ASSERT_TRUE(read.ok());
    EXPECT_TRUE(read.value().would_block);
    EXPECT_TRUE(read.value().timeline_discontinuity);
    EXPECT_EQ(2, observed->pcm_prepare_count);
    EXPECT_EQ(2, observed->pcm_start_count);
}

TEST(AlsaSystemTest, SuspendResumeNeverBlocksExecutor) {
    std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
    FakeAlsaApi* observed = fake.get();
    observed->pcm_read_results.push_back(-ESTRPIPE);
    observed->pcm_resume_results.push_back(-EAGAIN);
    observed->pcm_resume_results.push_back(0);
    eavp::detail::AlsaSystem system(std::move(fake));
    ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
    ASSERT_TRUE(system.start().ok());

    const eavp::Result<eavp::detail::AlsaReadResult> first =
        system.read_interleaved(reinterpret_cast<std::uint8_t*>(observed), 480);
    const eavp::Result<eavp::detail::AlsaReadResult> second =
        system.read_interleaved(reinterpret_cast<std::uint8_t*>(observed), 480);

    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_TRUE(first.value().would_block);
    EXPECT_TRUE(second.value().would_block);
    EXPECT_TRUE(first.value().timeline_discontinuity);
    EXPECT_FALSE(second.value().timeline_discontinuity);
    EXPECT_EQ(2, observed->pcm_resume_count);
}

TEST(AlsaSystemTest, SuspendResumeFatalFallsBackToPrepareAndStart) {
    std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
    FakeAlsaApi* observed = fake.get();
    observed->pcm_read_results.push_back(-ESTRPIPE);
    observed->pcm_resume_results.push_back(-EIO);
    eavp::detail::AlsaSystem system(std::move(fake));
    ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
    ASSERT_TRUE(system.start().ok());

    const eavp::Result<eavp::detail::AlsaReadResult> read =
        system.read_interleaved(reinterpret_cast<std::uint8_t*>(observed), 480);

    ASSERT_TRUE(read.ok());
    EXPECT_TRUE(read.value().would_block);
    EXPECT_TRUE(read.value().timeline_discontinuity);
    EXPECT_EQ(2, observed->pcm_prepare_count);
    EXPECT_EQ(2, observed->pcm_start_count);
}

TEST(AlsaSystemTest, SuspendResumeDeviceLossDoesNotAttemptFallback) {
    std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
    FakeAlsaApi* observed = fake.get();
    observed->pcm_read_results.push_back(-ESTRPIPE);
    observed->pcm_resume_results.push_back(-ENODEV);
    eavp::detail::AlsaSystem system(std::move(fake));
    ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
    ASSERT_TRUE(system.start().ok());

    const eavp::Result<eavp::detail::AlsaReadResult> read =
        system.read_interleaved(reinterpret_cast<std::uint8_t*>(observed), 480);

    ASSERT_FALSE(read.ok());
    EXPECT_EQ(eavp::StatusCode::kDeviceLost, read.status().code());
    EXPECT_EQ("snd_pcm_resume", read.status().operation());
    EXPECT_EQ(-ENODEV, read.status().native_code());
    EXPECT_EQ(1, observed->pcm_prepare_count);
    EXPECT_EQ(1, observed->pcm_start_count);
}

TEST(AlsaSystemTest, RecoveryFailureRetainsNativeOperationAndCode) {
    std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
    fake->pcm_read_results.push_back(-EPIPE);
    fake->pcm_prepare_results.push_back(0);
    fake->pcm_prepare_results.push_back(-EIO);
    eavp::detail::AlsaSystem system(std::move(fake));
    ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
    ASSERT_TRUE(system.start().ok());

    const eavp::Result<eavp::detail::AlsaReadResult> read =
        system.read_interleaved(reinterpret_cast<std::uint8_t*>(&system), 480);

    ASSERT_FALSE(read.ok());
    EXPECT_EQ(eavp::StatusCode::kIoError, read.status().code());
    EXPECT_EQ("alsa", read.status().provider_id());
    EXPECT_EQ("snd_pcm_prepare", read.status().operation());
    EXPECT_EQ(-EIO, read.status().native_code());
}

TEST(AlsaSystemTest, DeviceReadFailuresMapToDeviceLostWithoutReopen) {
    const int codes[] = {-ENODEV, -ENXIO};
    for (std::size_t index = 0U; index < 2U; ++index) {
        std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
        FakeAlsaApi* observed = fake.get();
        observed->pcm_read_results.push_back(codes[index]);
        eavp::detail::AlsaSystem system(std::move(fake));
        ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
        ASSERT_TRUE(system.start().ok());

        const eavp::Result<eavp::detail::AlsaReadResult> read =
            system.read_interleaved(reinterpret_cast<std::uint8_t*>(observed), 480);
        ASSERT_FALSE(read.ok());
        EXPECT_EQ(eavp::StatusCode::kDeviceLost, read.status().code());
        EXPECT_EQ("snd_pcm_readi", read.status().operation());
        EXPECT_EQ(codes[index], read.status().native_code());
        EXPECT_EQ(1, observed->successful_open_count);
    }
}

}  // namespace
