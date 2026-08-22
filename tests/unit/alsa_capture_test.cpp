#include <gtest/gtest.h>

#include <cerrno>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>

#include "eavp/management/health.hpp"
#include "eavp/management/metrics.hpp"
#include "eavp/media/port.hpp"
#include "eavp/media/audio_format.hpp"
#include "eavp/platform/linux/alsa_capture.hpp"
#include "platform/linux/alsa_capture_internal.hpp"
#include "support/audio_test_utils.hpp"

namespace {

eavp::AudioFormat make_test_audio_format() {
    return eavp::AudioFormat::create(
        eavp::SampleFormat::kSigned16LittleEndian, 48000,
        eavp::AudioChannelLayout::kStereo,
        eavp::AudioSampleLayout::kInterleaved,
        eavp::MemoryDomain::kCpu).take_value();
}

class NodeFixture {
public:
    NodeFixture(std::unique_ptr<eavp::detail::AlsaSystem> system,
                const eavp::AlsaCaptureConfig& config, std::size_t capacity = 4U,
                eavp::detail::AlsaObserver* observer = NULL)
        : metrics(), health(), input("audio_input", capacity,
                                     eavp::OverflowPolicy::kBlock), node() {
        node = eavp::detail::AlsaSourceNodeTestPeer::create(
            "mic0", config, &metrics, &health, std::move(system), observer).take_value();
        EXPECT_TRUE(eavp::connect(node->output(), input).ok());
    }

    bool start() { return node->prepare().ok() && node->start().ok(); }
    eavp::Status tick_once_running() { return node->tick(); }
    bool tick_until_frames(std::size_t count) {
        for (int index = 0; index < 64 && input.queue_size() < count; ++index) {
            const eavp::Status status = tick_once_running();
            if (!status.ok() && status.code() != eavp::StatusCode::kWouldBlock) {
                return false;
            }
        }
        return input.queue_size() >= count;
    }
    std::shared_ptr<const eavp::AudioFrame> take_frame() {
        return input.receive().take_value();
    }

    eavp::MetricRegistry metrics;
    eavp::HealthManager health;
    eavp::InputPort<eavp::AudioFrame> input;
    std::unique_ptr<eavp::AlsaSourceNode> node;
};

class FailingAlsaObserver : public eavp::detail::AlsaObserver {
public:
    FailingAlsaObserver() : fail_next_report(false) {}

    eavp::Status on_negotiated(int, int) override { return consume_failure(); }
    eavp::Status on_partial(int) override { return consume_failure(); }
    eavp::Status on_would_block() override { return consume_failure(); }
    eavp::Status on_frame(const eavp::AudioFrame&) override { return consume_failure(); }
    eavp::Status on_timestamp_fallback() override { return consume_failure(); }
    eavp::Status on_recovery(bool) override { return consume_failure(); }
    eavp::Status on_fatal(const eavp::Status&) override { return consume_failure(); }

    bool fail_next_report;

private:
    eavp::Status consume_failure() {
        if (!fail_next_report) return eavp::Status::ok_status();
        fail_next_report = false;
        return eavp::Status(eavp::StatusCode::kResourceExhausted,
                            "observer failed");
    }
};

class ThrowingAlsaObserver : public FailingAlsaObserver {
public:
    enum Failure { kBadAlloc, kUnexpected };

    explicit ThrowingAlsaObserver(Failure failure) : failure_(failure) {}

    eavp::Status on_would_block() override {
        if (failure_ == kBadAlloc) throw std::bad_alloc();
        throw std::runtime_error("observer threw");
    }

private:
    Failure failure_;
};

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

TEST(AlsaSourceNodeTest, AggregatesShortReadsIntoOneExactFrame) {
    eavp_test::ScriptedAlsa source;
    source.read_frames.push_back(120);
    source.read_frames.push_back(360);
    NodeFixture fixture(source.take_system(), eavp_test::make_alsa_config());
    ASSERT_TRUE(fixture.start());

    EXPECT_TRUE(fixture.node->tick().ok());
    EXPECT_EQ(0U, fixture.input.queue_size());
    EXPECT_EQ(1, source.observed_read_calls());
    EXPECT_TRUE(fixture.node->tick().ok());
    ASSERT_EQ(1U, fixture.input.queue_size());
    const std::shared_ptr<const eavp::AudioFrame> frame = fixture.take_frame();
    EXPECT_EQ(480, frame->samples_per_channel());
    EXPECT_EQ(1920U, frame->buffer().plane_layout(0U).value().size);
}

TEST(AlsaSourceNodeTest, PtsUsesFirstUnreadAnchorAndCumulativeSamples) {
    eavp_test::ScriptedAlsa source;
    source.set_initial_anchor(500000);
    source.append_complete_frames(3);
    NodeFixture fixture(source.take_system(), eavp_test::make_alsa_config(), 4U);
    ASSERT_TRUE(fixture.start());
    ASSERT_TRUE(fixture.tick_until_frames(3));

    EXPECT_EQ(500000, fixture.take_frame()->pts());
    EXPECT_EQ(510000, fixture.take_frame()->pts());
    EXPECT_EQ(520000, fixture.take_frame()->pts());
}

TEST(AlsaSourceNodeTest, PtsDiscardsWouldBlockAnchorCandidateAndResamplesBeforeRead) {
    std::unique_ptr<eavp_test::FakeAlsaApi> fake(new eavp_test::FakeAlsaApi());
    eavp_test::FakeAlsaApi* observed = fake.get();
    observed->htimestamp_value.tv_sec = 0;
    observed->htimestamp_value.tv_nsec = 500000000L;
    observed->pcm_read_results.push_back(0);
    observed->pcm_read_results.push_back(480);
    std::unique_ptr<eavp::detail::AlsaSystem> system(
        new eavp::detail::AlsaSystem(
            std::unique_ptr<eavp::detail::AlsaApi>(fake.release())));
    NodeFixture fixture(std::move(system), eavp_test::make_alsa_config(), 1U);
    ASSERT_TRUE(fixture.start());

    EXPECT_EQ(eavp::StatusCode::kWouldBlock, fixture.tick_once_running().code());
    ASSERT_TRUE(fixture.tick_once_running().ok());
    ASSERT_EQ(1U, fixture.input.queue_size());
    EXPECT_EQ(500000, fixture.take_frame()->pts());
    EXPECT_EQ(2, observed->htimestamp_count);
}

TEST(AlsaSourceNodeTest, XrunDropsPartialAndMarksExactlyOneFrame) {
    eavp_test::ScriptedAlsa source;
    source.set_initial_anchor(600000);
    source.set_recovery_anchor(700000);
    source.read_frames.push_back(240);
    source.read_errors.push_back(-EPIPE);
    source.append_complete_frames(2);
    NodeFixture fixture(source.take_system(), eavp_test::make_alsa_config(), 4U);
    ASSERT_TRUE(fixture.start());
    ASSERT_TRUE(fixture.tick_until_frames(2));

    EXPECT_TRUE(fixture.take_frame()->discontinuity());
    EXPECT_FALSE(fixture.take_frame()->discontinuity());
    EXPECT_EQ(1, source.prepare_after_xrun_calls());
    EXPECT_EQ(1, source.start_after_xrun_calls());
}

TEST(AlsaSourceNodeTest, PublishesFrameAndRecoveryObservability) {
    eavp_test::ScriptedAlsa source;
    source.read_frames.push_back(240);
    source.read_errors.push_back(-EPIPE);
    source.append_complete_frames(1);
    NodeFixture fixture(source.take_system(), eavp_test::make_alsa_config(), 2U);
    ASSERT_TRUE(fixture.start());
    ASSERT_TRUE(fixture.tick_until_frames(1));

    EXPECT_EQ(1U, fixture.metrics.counter("alsa_capture.mic0.frames").value());
    EXPECT_EQ(480U, fixture.metrics.counter("alsa_capture.mic0.samples").value());
    EXPECT_EQ(1920U, fixture.metrics.counter("alsa_capture.mic0.bytes").value());
    EXPECT_EQ(1U, fixture.metrics.counter("alsa_capture.mic0.short_reads").value());
    EXPECT_EQ(1U, fixture.metrics.counter("alsa_capture.mic0.xruns").value());
    EXPECT_EQ(1U, fixture.metrics.counter("alsa_capture.mic0.recoveries").value());
    EXPECT_EQ(1U, fixture.metrics.counter("alsa_capture.mic0.discontinuities").value());
    EXPECT_DOUBLE_EQ(0.0,
                     fixture.metrics.gauge("alsa_capture.mic0.partial_samples").value());
    EXPECT_EQ(eavp::HealthStatus::kDegraded,
              fixture.health.component("alsa_capture/mic0").value().status);
}

TEST(AlsaSourceNodeTest, PublishesNormalHealthAndNegotiatedGauges) {
    eavp_test::ScriptedAlsa source;
    NodeFixture fixture(source.take_system(), eavp_test::make_alsa_config());

    ASSERT_TRUE(fixture.start());
    EXPECT_EQ(eavp::HealthStatus::kOk,
              fixture.health.component("alsa_capture/mic0").value().status);
    EXPECT_DOUBLE_EQ(480.0,
                     fixture.metrics.gauge("alsa_capture.mic0.actual_period_frames")
                         .value());
    EXPECT_DOUBLE_EQ(1920.0,
                     fixture.metrics.gauge("alsa_capture.mic0.actual_buffer_frames")
                         .value());
}

TEST(AlsaSourceNodeTest, PublishesTimestampFallbackAndPartialSamples) {
    eavp_test::ScriptedAlsa source;
    source.force_timestamp_fallback();
    source.read_frames.push_back(240);
    NodeFixture fixture(source.take_system(), eavp_test::make_alsa_config());
    ASSERT_TRUE(fixture.start());

    ASSERT_TRUE(fixture.tick_once_running().ok());
    EXPECT_EQ(1U,
              fixture.metrics.counter("alsa_capture.mic0.timestamp_fallbacks").value());
    EXPECT_DOUBLE_EQ(240.0,
                     fixture.metrics.gauge("alsa_capture.mic0.partial_samples").value());
    EXPECT_EQ(eavp::HealthStatus::kDegraded,
              fixture.health.component("alsa_capture/mic0").value().status);
}

TEST(AlsaSourceNodeTest, PublishesSuspendRecoveryOnce) {
    eavp_test::ScriptedAlsa source;
    source.read_errors.push_back(-ESTRPIPE);
    source.resume_results.push_back(0);
    source.append_complete_frames(1);
    NodeFixture fixture(source.take_system(), eavp_test::make_alsa_config());
    ASSERT_TRUE(fixture.start());
    ASSERT_TRUE(fixture.tick_until_frames(1));

    EXPECT_EQ(1U, fixture.metrics.counter("alsa_capture.mic0.suspends").value());
    EXPECT_EQ(1U, fixture.metrics.counter("alsa_capture.mic0.recoveries").value());
    EXPECT_EQ(1U, fixture.metrics.counter("alsa_capture.mic0.discontinuities").value());
    EXPECT_EQ(2U, fixture.metrics.counter("alsa_capture.mic0.would_block").value());
    EXPECT_EQ(eavp::HealthStatus::kDegraded,
              fixture.health.component("alsa_capture/mic0").value().status);
}

TEST(AlsaSourceNodeTest, RecoveryClearsPartialSamplesGaugeBeforeTheNextFrame) {
    eavp_test::ScriptedAlsa source;
    source.read_frames.push_back(240);
    source.read_errors.push_back(-EPIPE);
    NodeFixture fixture(source.take_system(), eavp_test::make_alsa_config());
    ASSERT_TRUE(fixture.start());

    ASSERT_TRUE(fixture.tick_once_running().ok());
    EXPECT_DOUBLE_EQ(240.0,
                     fixture.metrics.gauge("alsa_capture.mic0.partial_samples").value());
    EXPECT_EQ(eavp::StatusCode::kWouldBlock, fixture.tick_once_running().code());
    EXPECT_DOUBLE_EQ(0.0,
                     fixture.metrics.gauge("alsa_capture.mic0.partial_samples").value());
}

TEST(AlsaSourceNodeTest, ResetRestoresOkHealthAfterRecovery) {
    eavp_test::ScriptedAlsa source;
    source.read_errors.push_back(-EPIPE);
    NodeFixture fixture(source.take_system(), eavp_test::make_alsa_config());
    ASSERT_TRUE(fixture.start());

    EXPECT_EQ(eavp::StatusCode::kWouldBlock, fixture.tick_once_running().code());
    ASSERT_EQ(eavp::HealthStatus::kDegraded,
              fixture.health.component("alsa_capture/mic0").value().status);
    ASSERT_TRUE(fixture.node->reset().ok());
    EXPECT_EQ(eavp::HealthStatus::kOk,
              fixture.health.component("alsa_capture/mic0").value().status);
}

TEST(AlsaSourceNodeTest, DeviceLostPublishesErrorHealth) {
    eavp_test::ScriptedAlsa source;
    source.read_errors.push_back(-ENODEV);
    NodeFixture fixture(source.take_system(), eavp_test::make_alsa_config());
    ASSERT_TRUE(fixture.start());

    EXPECT_EQ(eavp::StatusCode::kDeviceLost, fixture.tick_once_running().code());
    EXPECT_EQ(eavp::HealthStatus::kError,
              fixture.health.component("alsa_capture/mic0").value().status);
}

TEST(AlsaSourceNodeTest, MediaFailureWinsOverObserverFailure) {
    eavp_test::ScriptedAlsa source;
    source.read_errors.push_back(-ENODEV);
    FailingAlsaObserver observer;
    NodeFixture fixture(source.take_system(), eavp_test::make_alsa_config(), 2U,
                        &observer);
    ASSERT_TRUE(fixture.start());
    observer.fail_next_report = true;

    const eavp::Status status = fixture.tick_once_running();
    EXPECT_EQ(eavp::StatusCode::kDeviceLost, status.code());
    EXPECT_EQ("alsa", status.provider_id());
}

TEST(AlsaSourceNodeTest, PropagatesObserverFailureWithoutMediaFailure) {
    eavp_test::ScriptedAlsa source;
    source.append_complete_frames(1);
    FailingAlsaObserver observer;
    NodeFixture fixture(source.take_system(), eavp_test::make_alsa_config(), 2U,
                        &observer);
    ASSERT_TRUE(fixture.start());
    observer.fail_next_report = true;

    EXPECT_EQ(eavp::StatusCode::kResourceExhausted,
              fixture.tick_once_running().code());
}

TEST(AlsaSourceNodeTest, MapsObserverExceptionsAtTheNodeBoundary) {
    eavp_test::ScriptedAlsa bad_alloc_source;
    bad_alloc_source.read_frames.push_back(0);
    ThrowingAlsaObserver bad_alloc_observer(ThrowingAlsaObserver::kBadAlloc);
    NodeFixture bad_alloc_fixture(bad_alloc_source.take_system(),
                                 eavp_test::make_alsa_config(), 2U,
                                 &bad_alloc_observer);
    ASSERT_TRUE(bad_alloc_fixture.start());
    EXPECT_EQ(eavp::StatusCode::kResourceExhausted,
              bad_alloc_fixture.tick_once_running().code());

    eavp_test::ScriptedAlsa unexpected_source;
    unexpected_source.read_frames.push_back(0);
    ThrowingAlsaObserver unexpected_observer(ThrowingAlsaObserver::kUnexpected);
    NodeFixture unexpected_fixture(unexpected_source.take_system(),
                                  eavp_test::make_alsa_config(), 2U,
                                  &unexpected_observer);
    ASSERT_TRUE(unexpected_fixture.start());
    EXPECT_EQ(eavp::StatusCode::kInternal,
              unexpected_fixture.tick_once_running().code());
}

TEST(AlsaSourceNodeTest, SuspendRecoveryResumesBeforeSamplingANewAnchor) {
    std::unique_ptr<eavp_test::FakeAlsaApi> fake(new eavp_test::FakeAlsaApi());
    eavp_test::FakeAlsaApi* observed = fake.get();
    observed->pcm_read_results.push_back(-ESTRPIPE);
    observed->pcm_read_results.push_back(480);
    observed->pcm_resume_results.push_back(-EAGAIN);
    observed->pcm_resume_results.push_back(0);
    std::unique_ptr<eavp::detail::AlsaSystem> system(
        new eavp::detail::AlsaSystem(
            std::unique_ptr<eavp::detail::AlsaApi>(fake.release())));
    NodeFixture fixture(std::move(system), eavp_test::make_alsa_config(), 1U);
    ASSERT_TRUE(fixture.start());

    EXPECT_EQ(eavp::StatusCode::kWouldBlock, fixture.tick_once_running().code());
    EXPECT_EQ(1, observed->htimestamp_count);
    EXPECT_EQ(eavp::StatusCode::kWouldBlock, fixture.tick_once_running().code());
    EXPECT_EQ(1, observed->htimestamp_count);
    EXPECT_EQ(eavp::StatusCode::kWouldBlock, fixture.tick_once_running().code());
    EXPECT_EQ(1, observed->htimestamp_count);
    ASSERT_TRUE(fixture.tick_once_running().ok());
    EXPECT_EQ(2, observed->htimestamp_count);
    ASSERT_EQ(1U, fixture.input.queue_size());
    EXPECT_TRUE(fixture.take_frame()->discontinuity());
}

TEST(AlsaSourceNodeTest, PendingOutputStopsFurtherDeviceReads) {
    eavp_test::ScriptedAlsa source;
    source.append_complete_frames(3);
    NodeFixture fixture(source.take_system(), eavp_test::make_alsa_config(), 1U);
    ASSERT_TRUE(fixture.start());
    ASSERT_TRUE(fixture.node->tick().ok());
    ASSERT_EQ(eavp::StatusCode::kWouldBlock, fixture.node->tick().code());
    const int reads_before_blocked_retry = source.observed_read_calls();
    EXPECT_EQ(2, reads_before_blocked_retry);

    EXPECT_EQ(eavp::StatusCode::kWouldBlock, fixture.node->tick().code());
    EXPECT_EQ(reads_before_blocked_retry, source.observed_read_calls());
}

TEST(AlsaSystemReadTest, ZeroAndEagainReturnWouldBlock) {
    eavp_test::ScriptedAlsa zero_source;
    zero_source.read_frames.push_back(0);
    eavp::detail::AlsaSystem zero_system = zero_source.take_started_system();
    const eavp::Result<eavp::detail::AlsaReadResult> zero =
        zero_system.read_interleaved(zero_source.bytes(), 480);
    ASSERT_TRUE(zero.ok());
    EXPECT_TRUE(zero.value().would_block);

    eavp_test::ScriptedAlsa eagain_source;
    eagain_source.append_error(-EAGAIN);
    eavp::detail::AlsaSystem eagain_system = eagain_source.take_started_system();
    const eavp::Result<eavp::detail::AlsaReadResult> eagain =
        eagain_system.read_interleaved(eagain_source.bytes(), 480);
    ASSERT_TRUE(eagain.ok());
    EXPECT_TRUE(eagain.value().would_block);
}

TEST(AlsaSystemReadTest, EintrRetriesSameRead) {
    eavp_test::ScriptedAlsa source;
    source.append_error(-EINTR);
    source.read_frames.push_back(120);
    eavp::detail::AlsaSystem system = source.take_started_system();

    const eavp::Result<eavp::detail::AlsaReadResult> result =
        system.read_interleaved(source.bytes(), 480);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(120, result.value().frames_read);
    EXPECT_EQ(2, source.observed_read_calls());
}

TEST(AlsaSystemReadTest, RejectsInvalidRequests) {
    eavp_test::ScriptedAlsa source;
    std::unique_ptr<eavp::detail::AlsaSystem> stopped_holder = source.take_system();
    eavp::detail::AlsaSystem stopped = std::move(*stopped_holder);
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              stopped.read_interleaved(source.bytes(), 480).status().code());

    eavp::detail::AlsaSystem running = source.take_started_system();
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              running.read_interleaved(NULL, 480).status().code());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              running.read_interleaved(source.bytes(), 0).status().code());
}

TEST(AlsaSystemReadTest, RejectsDriverFrameCountsOutsideRequest) {
    eavp_test::ScriptedAlsa source;
    source.read_frames.push_back(481);
    eavp::detail::AlsaSystem system = source.take_started_system();
    EXPECT_EQ(eavp::StatusCode::kCorruptData,
              system.read_interleaved(source.bytes(), 480).status().code());
}

TEST(AlsaSystemReadTest, RejectsDriverFrameCountOutsideIntRange) {
    if (std::numeric_limits<snd_pcm_sframes_t>::max() <=
        std::numeric_limits<int>::max()) {
        GTEST_SKIP() << "native ALSA frame count cannot exceed int on this target";
    }
    eavp_test::ScriptedAlsa source;
    source.read_frames.push_back(
        static_cast<snd_pcm_sframes_t>(std::numeric_limits<int>::max()) + 1);
    eavp::detail::AlsaSystem system = source.take_started_system();
    EXPECT_EQ(eavp::StatusCode::kCorruptData,
              system.read_interleaved(source.bytes(), 480).status().code());
}

TEST(AlsaSystemReadTest, MapsFatalReadFailure) {
    eavp_test::ScriptedAlsa source;
    source.append_error(-EIO);
    eavp::detail::AlsaSystem system = source.take_started_system();
    const eavp::Result<eavp::detail::AlsaReadResult> result =
        system.read_interleaved(source.bytes(), 480);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(eavp::StatusCode::kIoError, result.status().code());
    EXPECT_EQ("alsa", result.status().provider_id());
    EXPECT_EQ("snd_pcm_readi", result.status().operation());
    EXPECT_EQ(-EIO, result.status().native_code());
}

TEST(AlsaSourceNodeTest, StopDiscardsPartialAndPending) {
    eavp_test::ScriptedAlsa source;
    source.read_frames.push_back(240);
    source.read_frames.push_back(240);
    source.append_complete_frames(1);
    source.read_frames.push_back(240);
    source.read_frames.push_back(240);
    NodeFixture fixture(source.take_system(), eavp_test::make_alsa_config(), 1U);
    ASSERT_TRUE(fixture.start());
    ASSERT_TRUE(fixture.node->tick().ok());
    ASSERT_TRUE(fixture.node->tick().ok());
    ASSERT_EQ(eavp::StatusCode::kWouldBlock, fixture.node->tick().code());
    ASSERT_TRUE(fixture.node->stop().ok());
    ASSERT_TRUE(fixture.node->reset().ok());
    ASSERT_TRUE(fixture.node->prepare().ok());
    ASSERT_TRUE(fixture.node->start().ok());
    ASSERT_TRUE(fixture.input.receive().ok());

    ASSERT_TRUE(fixture.node->tick().ok());
    EXPECT_EQ(0U, fixture.input.queue_size());
    ASSERT_TRUE(fixture.node->tick().ok());
    EXPECT_EQ(1U, fixture.input.queue_size());
}

TEST(AlsaSourceNodeTest, FactoryRejectsEmptyIdOrObservers) {
    const eavp::AlsaCaptureConfig config = eavp_test::make_alsa_config();
    eavp::MetricRegistry metrics;
    eavp::HealthManager health;

    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::AlsaSourceNode::create("", config, &metrics, &health)
                  .status().code());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::AlsaSourceNode::create("mic0", config, NULL, &health)
                  .status().code());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::AlsaSourceNode::create("mic0", config, &metrics, NULL)
                  .status().code());
}

}  // namespace
