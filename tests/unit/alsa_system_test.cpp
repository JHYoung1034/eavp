#include <cerrno>
#include <limits>
#include <memory>
#include <poll.h>
#include <vector>

#include <gtest/gtest.h>

#include "platform/linux/alsa_system.hpp"
#include "support/audio_test_utils.hpp"
#include "support/fake_alsa_api.hpp"

namespace {

using eavp_test::FakeAlsaApi;
using eavp_test::make_alsa_config;

struct pollfd descriptor(int fd, short events) {
    const struct pollfd value = {fd, events, 0};
    return value;
}

TEST(AlsaSystemTest, PreservesCompletePollDescriptorArrayAndDecodesEvents) {
    std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
    FakeAlsaApi* observed = fake.get();
    observed->poll_descriptor_values.push_back(descriptor(20, POLLIN));
    observed->poll_descriptor_values.push_back(descriptor(21, POLLOUT));
    observed->poll_revents_value = POLLIN;
    eavp::detail::AlsaSystem system(std::move(fake));
    ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
    ASSERT_TRUE(system.start().ok());

    eavp::Result<std::vector<struct pollfd> > descriptors =
        system.poll_descriptors();

    ASSERT_TRUE(descriptors.ok());
    ASSERT_EQ(2U, descriptors.value().size());
    EXPECT_EQ(20, descriptors.value()[0].fd);
    EXPECT_EQ(POLLIN, descriptors.value()[0].events);
    EXPECT_EQ(21, descriptors.value()[1].fd);
    EXPECT_EQ(POLLOUT, descriptors.value()[1].events);
    descriptors.value()[0].revents = POLLIN;
    eavp::Result<bool> ready =
        system.evaluate_poll_events(descriptors.value());
    ASSERT_TRUE(ready.ok());
    EXPECT_TRUE(ready.value());
    EXPECT_EQ(1, observed->poll_descriptors_count_calls);
    EXPECT_EQ(1, observed->poll_descriptors_calls);
    EXPECT_EQ(1, observed->poll_revents_calls);
    ASSERT_EQ(2U, observed->last_poll_revents_descriptors.size());
    EXPECT_EQ(POLLIN, observed->last_poll_revents_descriptors[0].revents);
    EXPECT_EQ(21, observed->last_poll_revents_descriptors[1].fd);
}

TEST(AlsaSystemTest, RejectsZeroAndMapsNegativePollDescriptorCounts) {
    {
        std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
        fake->poll_descriptor_count_results.push_back(0);
        eavp::detail::AlsaSystem system(std::move(fake));
        ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
        ASSERT_TRUE(system.start().ok());

        const eavp::Result<std::vector<struct pollfd> > result =
            system.poll_descriptors();
        ASSERT_FALSE(result.ok());
        EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch,
                  result.status().code());
        EXPECT_EQ("alsa", result.status().provider_id());
        EXPECT_EQ("snd_pcm_poll_descriptors_count",
                  result.status().operation());
        EXPECT_EQ(0, result.status().native_code());
    }
    {
        std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
        fake->poll_descriptor_count_results.push_back(-ENODEV);
        eavp::detail::AlsaSystem system(std::move(fake));
        ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
        ASSERT_TRUE(system.start().ok());

        const eavp::Result<std::vector<struct pollfd> > result =
            system.poll_descriptors();
        ASSERT_FALSE(result.ok());
        EXPECT_EQ(eavp::StatusCode::kDeviceLost, result.status().code());
        EXPECT_EQ("alsa", result.status().provider_id());
        EXPECT_EQ("snd_pcm_poll_descriptors_count",
                  result.status().operation());
        EXPECT_EQ(-ENODEV, result.status().native_code());
    }
}

TEST(AlsaSystemTest, RejectsPollDescriptorArrayLengthChangesAndApiFailures) {
    {
        std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
        fake->poll_descriptor_values.push_back(descriptor(20, POLLIN));
        fake->poll_descriptor_values.push_back(descriptor(21, POLLOUT));
        fake->poll_descriptor_results.push_back(1);
        eavp::detail::AlsaSystem system(std::move(fake));
        ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
        ASSERT_TRUE(system.start().ok());

        const eavp::Result<std::vector<struct pollfd> > result =
            system.poll_descriptors();
        ASSERT_FALSE(result.ok());
        EXPECT_EQ(eavp::StatusCode::kCorruptData, result.status().code());
        EXPECT_EQ("alsa", result.status().provider_id());
        EXPECT_EQ("snd_pcm_poll_descriptors", result.status().operation());
        EXPECT_EQ(1, result.status().native_code());
    }
    {
        std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
        fake->poll_descriptor_values.push_back(descriptor(20, POLLIN));
        fake->poll_descriptor_results.push_back(-EIO);
        eavp::detail::AlsaSystem system(std::move(fake));
        ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
        ASSERT_TRUE(system.start().ok());

        const eavp::Result<std::vector<struct pollfd> > result =
            system.poll_descriptors();
        ASSERT_FALSE(result.ok());
        EXPECT_EQ(eavp::StatusCode::kIoError, result.status().code());
        EXPECT_EQ("snd_pcm_poll_descriptors", result.status().operation());
        EXPECT_EQ(-EIO, result.status().native_code());
    }
}

TEST(AlsaSystemTest, RequiresTheRegisteredDescriptorCountWhenDecodingEvents) {
    std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
    FakeAlsaApi* observed = fake.get();
    observed->poll_descriptor_values.push_back(descriptor(20, POLLIN));
    observed->poll_descriptor_values.push_back(descriptor(21, POLLOUT));
    eavp::detail::AlsaSystem system(std::move(fake));
    ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
    ASSERT_TRUE(system.start().ok());
    ASSERT_TRUE(system.poll_descriptors().ok());

    const std::vector<struct pollfd> changed(1U, descriptor(20, POLLIN));
    const eavp::Result<bool> result = system.evaluate_poll_events(changed);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument, result.status().code());
    EXPECT_EQ(0, observed->poll_revents_calls);
}

TEST(AlsaSystemTest, RejectsDescriptorCountChangesAfterTheFirstFetch) {
    std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
    FakeAlsaApi* observed = fake.get();
    observed->poll_descriptor_values.push_back(descriptor(20, POLLIN));
    observed->poll_descriptor_values.push_back(descriptor(21, POLLOUT));
    eavp::detail::AlsaSystem system(std::move(fake));
    ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
    ASSERT_TRUE(system.start().ok());
    ASSERT_TRUE(system.poll_descriptors().ok());
    observed->poll_descriptor_values.resize(1U);

    const eavp::Result<std::vector<struct pollfd> > changed =
        system.poll_descriptors();

    ASSERT_FALSE(changed.ok());
    EXPECT_EQ(eavp::StatusCode::kCorruptData, changed.status().code());
    EXPECT_EQ("alsa", changed.status().provider_id());
    EXPECT_EQ("snd_pcm_poll_descriptors_count", changed.status().operation());
    EXPECT_EQ(1, changed.status().native_code());
}

TEST(AlsaSystemTest, MapsPollReventsErrorsAndDeviceLossWithoutGuessingBits) {
    {
        std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
        fake->poll_descriptor_values.push_back(descriptor(20, POLLIN));
        fake->poll_revents_results.push_back(-EIO);
        eavp::detail::AlsaSystem system(std::move(fake));
        ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
        ASSERT_TRUE(system.start().ok());
        eavp::Result<std::vector<struct pollfd> > descriptors =
            system.poll_descriptors();
        ASSERT_TRUE(descriptors.ok());

        const eavp::Result<bool> result =
            system.evaluate_poll_events(descriptors.value());
        ASSERT_FALSE(result.ok());
        EXPECT_EQ(eavp::StatusCode::kIoError, result.status().code());
        EXPECT_EQ("alsa", result.status().provider_id());
        EXPECT_EQ("snd_pcm_poll_descriptors_revents",
                  result.status().operation());
        EXPECT_EQ(-EIO, result.status().native_code());
    }

    const unsigned short lost_events[] = {POLLHUP, POLLNVAL};
    for (std::size_t index = 0U; index < 2U; ++index) {
        std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
        fake->poll_descriptor_values.push_back(descriptor(20, POLLIN));
        fake->poll_revents_value = static_cast<unsigned short>(
            lost_events[index] | POLLIN | POLLERR);
        eavp::detail::AlsaSystem system(std::move(fake));
        ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
        ASSERT_TRUE(system.start().ok());
        eavp::Result<std::vector<struct pollfd> > descriptors =
            system.poll_descriptors();
        ASSERT_TRUE(descriptors.ok());

        const eavp::Result<bool> result =
            system.evaluate_poll_events(descriptors.value());
        ASSERT_FALSE(result.ok());
        EXPECT_EQ(eavp::StatusCode::kDeviceLost, result.status().code());
        EXPECT_EQ("alsa", result.status().provider_id());
        EXPECT_EQ("snd_pcm_poll_descriptors_revents",
                  result.status().operation());
        EXPECT_EQ(static_cast<int>(lost_events[index] | POLLIN | POLLERR),
                  result.status().native_code());
    }
}

TEST(AlsaSystemTest, TreatsDecodedReadWriteOrErrorBitsAsReady) {
    const unsigned short ready_events[] = {POLLIN, POLLOUT, POLLERR};
    for (std::size_t index = 0U; index < 3U; ++index) {
        std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
        fake->poll_descriptor_values.push_back(descriptor(20, POLLIN));
        fake->poll_revents_value = ready_events[index];
        eavp::detail::AlsaSystem system(std::move(fake));
        ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
        ASSERT_TRUE(system.start().ok());
        eavp::Result<std::vector<struct pollfd> > descriptors =
            system.poll_descriptors();
        ASSERT_TRUE(descriptors.ok());

        const eavp::Result<bool> result =
            system.evaluate_poll_events(descriptors.value());
        ASSERT_TRUE(result.ok());
        EXPECT_TRUE(result.value());
    }
}

TEST(AlsaSystemTest, ReadinessRequiresRunningStateAndSuccessfulDescriptorFetch) {
    std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
    fake->poll_descriptor_values.push_back(descriptor(20, POLLIN));
    eavp::detail::AlsaSystem system(std::move(fake));
    const std::vector<struct pollfd> descriptors(1U, descriptor(20, POLLIN));

    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              system.poll_descriptors().status().code());
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              system.evaluate_poll_events(descriptors).status().code());
    ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              system.poll_descriptors().status().code());
    ASSERT_TRUE(system.start().ok());
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              system.evaluate_poll_events(descriptors).status().code());
    ASSERT_TRUE(system.poll_descriptors().ok());
    ASSERT_TRUE(system.stop().ok());
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              system.poll_descriptors().status().code());
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              system.evaluate_poll_events(descriptors).status().code());
}

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
    const int expected_open_mode =
        SND_PCM_NONBLOCK | SND_PCM_NO_AUTO_RESAMPLE |
        SND_PCM_NO_AUTO_CHANNELS | SND_PCM_NO_AUTO_FORMAT;
    EXPECT_EQ(expected_open_mode, observed->opened_mode);
    EXPECT_NE(0, observed->opened_mode & SND_PCM_NONBLOCK);
    EXPECT_NE(0, observed->opened_mode & SND_PCM_NO_AUTO_RESAMPLE);
    EXPECT_NE(0, observed->opened_mode & SND_PCM_NO_AUTO_CHANNELS);
    EXPECT_NE(0, observed->opened_mode & SND_PCM_NO_AUTO_FORMAT);
    EXPECT_EQ(SND_PCM_ACCESS_RW_INTERLEAVED, observed->requested_access);
    EXPECT_EQ(SND_PCM_FORMAT_S16_LE, observed->requested_format);
    EXPECT_EQ(48000U, observed->requested_rate);
    EXPECT_EQ(2U, observed->requested_channels);
    EXPECT_EQ(512U, system.negotiated().period_frames);
    EXPECT_EQ(2048U, system.negotiated().buffer_frames);
    EXPECT_TRUE(system.negotiated().monotonic_timestamp);
}

TEST(AlsaSystemTest, MapsMonoAndStereoChannelCountsExactly) {
    struct Case {
        eavp::AudioChannelLayout layout;
        unsigned int channels;
    };
    const Case cases[] = {
        {eavp::AudioChannelLayout::kMono, 1U},
        {eavp::AudioChannelLayout::kStereo, 2U},
    };
    for (std::size_t index = 0U; index < 2U; ++index) {
        std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
        FakeAlsaApi* observed = fake.get();
        observed->negotiated_channels = cases[index].channels;
        eavp::detail::AlsaSystem system(std::move(fake));

        ASSERT_TRUE(system.prepare(make_alsa_config(
            eavp::SampleFormat::kSigned16LittleEndian,
            cases[index].layout)).ok());
        EXPECT_EQ(cases[index].channels, observed->requested_channels);
    }
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

TEST(AlsaSystemTest, UnsupportedMonotonicTimestampUsesMonotonicAvailFallback) {
    std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
    FakeAlsaApi* observed = fake.get();
    observed->fail_step = FakeAlsaApi::kSwParamsSetTstampType;
    observed->fail_code = -EINVAL;
    observed->htimestamp_value.tv_sec = 9;
    observed->avail_update_result = 480;
    observed->monotonic_now_value.tv_sec = 3;
    eavp::detail::AlsaSystem system(std::move(fake));

    ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
    ASSERT_TRUE(system.start().ok());
    EXPECT_FALSE(system.negotiated().monotonic_timestamp);
    const eavp::Result<eavp::detail::AlsaAnchor> anchor = system.capture_anchor();
    ASSERT_TRUE(anchor.ok());
    EXPECT_EQ(2990000, anchor.value().first_unread_pts_us);
    EXPECT_TRUE(anchor.value().used_fallback);
    EXPECT_EQ(0, observed->htimestamp_count);
    EXPECT_EQ(1, observed->avail_update_count);
    EXPECT_EQ(1, observed->monotonic_now_count);
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

TEST(AlsaSystemTest,
     AnchorQueryRecoversXrunAndDefersSuspendForBothTimestampPaths) {
    const int errors[] = {-EPIPE, -ESTRPIPE};
    for (std::size_t fallback_index = 0U; fallback_index < 2U;
         ++fallback_index) {
        for (std::size_t error_index = 0U; error_index < 2U; ++error_index) {
            SCOPED_TRACE(::testing::Message()
                         << "fallback=" << fallback_index
                         << ", error=" << errors[error_index]);
            std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
            FakeAlsaApi* observed = fake.get();
            if (fallback_index != 0U) {
                observed->fail_step = FakeAlsaApi::kSwParamsSetTstampType;
                observed->fail_code = -EINVAL;
                observed->avail_update_result = errors[error_index];
            } else {
                observed->htimestamp_result = errors[error_index];
            }
            eavp::detail::AlsaSystem system(std::move(fake));
            ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
            ASSERT_TRUE(system.start().ok());

            const eavp::Result<eavp::detail::AlsaAnchor> anchor =
                system.capture_anchor();

            ASSERT_TRUE(anchor.ok());
            EXPECT_EQ(eavp::detail::AlsaAnchor::kTimelineDiscontinuity,
                      anchor.value().outcome);
            if (errors[error_index] == -EPIPE) {
                EXPECT_FALSE(system.suspend_recovery_pending());
                EXPECT_EQ(2, observed->pcm_prepare_count);
                EXPECT_EQ(2, observed->pcm_start_count);
            } else {
                EXPECT_TRUE(system.suspend_recovery_pending());
                EXPECT_EQ(1, observed->pcm_prepare_count);
                EXPECT_EQ(1, observed->pcm_start_count);
                EXPECT_EQ(0, observed->pcm_resume_count);
            }
        }
    }
}

TEST(AlsaSystemTest, AnchorQueryEagainReturnsWouldBlockWithoutRecovery) {
    std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
    FakeAlsaApi* observed = fake.get();
    observed->fail_step = FakeAlsaApi::kSwParamsSetTstampType;
    observed->fail_code = -EINVAL;
    observed->avail_update_result = -EAGAIN;
    eavp::detail::AlsaSystem system(std::move(fake));
    ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
    ASSERT_TRUE(system.start().ok());

    const eavp::Result<eavp::detail::AlsaAnchor> anchor =
        system.capture_anchor();

    ASSERT_TRUE(anchor.ok());
    EXPECT_EQ(eavp::detail::AlsaAnchor::kWouldBlock,
              anchor.value().outcome);
    EXPECT_FALSE(system.suspend_recovery_pending());
    EXPECT_EQ(1, observed->pcm_prepare_count);
    EXPECT_EQ(1, observed->pcm_start_count);
}

TEST(AlsaSystemTest, AnchorQueryDeviceLossNeverFallsBack) {
    const int errors[] = {-ENODEV, -ENXIO};
    for (std::size_t fallback_index = 0U; fallback_index < 2U;
         ++fallback_index) {
        for (std::size_t error_index = 0U; error_index < 2U; ++error_index) {
            SCOPED_TRACE(::testing::Message()
                         << "fallback=" << fallback_index
                         << ", error=" << errors[error_index]);
            std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
            FakeAlsaApi* observed = fake.get();
            if (fallback_index != 0U) {
                observed->fail_step = FakeAlsaApi::kSwParamsSetTstampType;
                observed->fail_code = -EINVAL;
                observed->avail_update_result = errors[error_index];
            } else {
                observed->htimestamp_result = errors[error_index];
            }
            eavp::detail::AlsaSystem system(std::move(fake));
            ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
            ASSERT_TRUE(system.start().ok());

            const eavp::Result<eavp::detail::AlsaAnchor> anchor =
                system.capture_anchor();

            ASSERT_FALSE(anchor.ok());
            EXPECT_EQ(eavp::StatusCode::kDeviceLost, anchor.status().code());
            EXPECT_EQ(errors[error_index], anchor.status().native_code());
            EXPECT_EQ(fallback_index == 0U ? "snd_pcm_htimestamp"
                                           : "snd_pcm_avail_update",
                      anchor.status().operation());
            if (fallback_index == 0U) {
                EXPECT_EQ(0, observed->avail_update_count);
            }
        }
    }
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
    ASSERT_TRUE(first.ok());
    EXPECT_TRUE(first.value().would_block);
    EXPECT_TRUE(first.value().timeline_discontinuity);
    EXPECT_EQ(0, observed->pcm_resume_count);
    const eavp::Result<eavp::detail::AlsaReadResult> second =
        system.read_interleaved(reinterpret_cast<std::uint8_t*>(observed), 480);
    ASSERT_TRUE(second.ok());
    EXPECT_TRUE(second.value().would_block);
    EXPECT_FALSE(second.value().timeline_discontinuity);
    EXPECT_EQ(1, observed->pcm_resume_count);
    const eavp::Result<eavp::detail::AlsaReadResult> third =
        system.read_interleaved(reinterpret_cast<std::uint8_t*>(observed), 480);

    ASSERT_TRUE(third.ok());
    EXPECT_TRUE(third.value().would_block);
    EXPECT_FALSE(third.value().timeline_discontinuity);
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

    const eavp::Result<eavp::detail::AlsaReadResult> first =
        system.read_interleaved(reinterpret_cast<std::uint8_t*>(observed), 480);
    ASSERT_TRUE(first.ok());
    EXPECT_TRUE(first.value().would_block);
    EXPECT_TRUE(first.value().timeline_discontinuity);
    EXPECT_EQ(0, observed->pcm_resume_count);
    EXPECT_EQ(1, observed->pcm_prepare_count);
    EXPECT_EQ(1, observed->pcm_start_count);

    const eavp::Result<eavp::detail::AlsaReadResult> second =
        system.read_interleaved(reinterpret_cast<std::uint8_t*>(observed), 480);

    ASSERT_TRUE(second.ok());
    EXPECT_TRUE(second.value().would_block);
    EXPECT_FALSE(second.value().timeline_discontinuity);
    EXPECT_EQ(1, observed->pcm_resume_count);
    EXPECT_EQ(2, observed->pcm_prepare_count);
    EXPECT_EQ(2, observed->pcm_start_count);
}

TEST(AlsaSystemTest, SuspendResumeDeviceLossDoesNotAttemptFallback) {
    const int codes[] = {-ENODEV, -ENXIO};
    for (std::size_t index = 0U; index < 2U; ++index) {
        std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
        FakeAlsaApi* observed = fake.get();
        observed->pcm_read_results.push_back(-ESTRPIPE);
        observed->pcm_resume_results.push_back(codes[index]);
        eavp::detail::AlsaSystem system(std::move(fake));
        ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
        ASSERT_TRUE(system.start().ok());

        ASSERT_TRUE(system.read_interleaved(
            reinterpret_cast<std::uint8_t*>(observed), 480).ok());
        const eavp::Result<eavp::detail::AlsaReadResult> read =
            system.read_interleaved(reinterpret_cast<std::uint8_t*>(observed), 480);

        ASSERT_FALSE(read.ok());
        EXPECT_EQ(eavp::StatusCode::kDeviceLost, read.status().code());
        EXPECT_EQ("snd_pcm_resume", read.status().operation());
        EXPECT_EQ(codes[index], read.status().native_code());
        EXPECT_EQ(1, observed->pcm_resume_count);
        EXPECT_EQ(1, observed->pcm_prepare_count);
        EXPECT_EQ(1, observed->pcm_start_count);
    }
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
