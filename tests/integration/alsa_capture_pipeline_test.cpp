#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "eavp/management/health.hpp"
#include "eavp/management/metrics.hpp"
#include "eavp/media/pipeline.hpp"
#include "eavp/media/port.hpp"
#include "eavp/platform/linux/alsa_capture.hpp"
#include "platform/linux/alsa_capture_internal.hpp"
#include "support/audio_test_utils.hpp"

namespace {

const std::uint64_t kFnv1aOffsetBasis = 14695981039346656037ULL;
const std::uint64_t kFnv1aPrime = 1099511628211ULL;

void append_generated_bytes(std::uint64_t* checksum, std::uint8_t* next_byte,
                            std::size_t count) {
    for (std::size_t index = 0U; index < count; ++index) {
        *checksum ^= static_cast<std::uint64_t>(*next_byte);
        *checksum *= kFnv1aPrime;
        ++(*next_byte);
    }
}

void discard_generated_bytes(std::uint8_t* next_byte, std::size_t count) {
    for (std::size_t index = 0U; index < count; ++index) {
        ++(*next_byte);
    }
}

std::uint64_t expected_checksum() {
    std::uint64_t checksum = kFnv1aOffsetBasis;
    std::uint8_t next_byte = 0U;
    for (int index = 0; index < 150; ++index) {
        append_generated_bytes(&checksum, &next_byte, 120U * 4U);
        append_generated_bytes(&checksum, &next_byte, 360U * 4U);
    }
    discard_generated_bytes(&next_byte, 240U * 4U);
    for (int index = 0; index < 150; ++index) {
        append_generated_bytes(&checksum, &next_byte, 200U * 4U);
        append_generated_bytes(&checksum, &next_byte, 280U * 4U);
    }
    return checksum;
}

class AudioChecksumSink : public eavp::MediaNode {
public:
    AudioChecksumSink()
        : eavp::MediaNode("audio-checksum"),
          input_("audio-input", 4U, eavp::OverflowPolicy::kBlock), frames_(0U),
          samples_(0U), checksum_(kFnv1aOffsetBasis), discontinuities_(0U),
          has_previous_pts_(false), previous_pts_(0), pts_steps_are_ten_ms_(true) {}

    eavp::InputPort<eavp::AudioFrame>& input() { return input_; }
    std::size_t frames() const { return frames_; }
    std::size_t samples() const { return samples_; }
    std::uint64_t checksum() const { return checksum_; }
    std::size_t discontinuities() const { return discontinuities_; }
    bool pts_steps_are_ten_ms() const { return pts_steps_are_ten_ms_; }

protected:
    eavp::Status on_tick() override {
        eavp::Result<std::shared_ptr<const eavp::AudioFrame> > received =
            input_.receive();
        if (!received.ok()) return received.status();
        const std::shared_ptr<const eavp::AudioFrame> frame = received.take_value();
        eavp::Result<eavp::MappedRegion> mapped = frame->buffer().map_plane(
            0U, eavp::MapMode::kReadOnly);
        if (!mapped.ok()) return mapped.status();
        const eavp::MappedRegion bytes = mapped.take_value();
        for (std::size_t index = 0U; index < bytes.size(); ++index) {
            checksum_ ^= static_cast<std::uint64_t>(bytes.data()[index]);
            checksum_ *= kFnv1aPrime;
        }
        if (has_previous_pts_ && !frame->discontinuity() &&
            frame->pts() - previous_pts_ != 10000) {
            pts_steps_are_ten_ms_ = false;
        }
        previous_pts_ = frame->pts();
        has_previous_pts_ = true;
        if (frame->discontinuity()) ++discontinuities_;
        samples_ += static_cast<std::size_t>(frame->samples_per_channel());
        ++frames_;
        return eavp::Status::ok_status();
    }

private:
    eavp::InputPort<eavp::AudioFrame> input_;
    std::size_t frames_;
    std::size_t samples_;
    std::uint64_t checksum_;
    std::size_t discontinuities_;
    bool has_previous_pts_;
    std::int64_t previous_pts_;
    bool pts_steps_are_ten_ms_;
};

TEST(AlsaCapturePipelineTest,
     DeliversThreeHundredExactFramesAcrossShortReadsAndOneXrun) {
    eavp_test::ScriptedAlsa source;
    source.append_patterned_frames(150, 120, 360);
    source.read_frames.push_back(240);
    source.append_error(-EPIPE);
    source.set_recovery_anchor(9000000);
    source.append_patterned_frames(150, 200, 280);

    eavp::MetricRegistry metrics;
    eavp::HealthManager health;
    std::unique_ptr<eavp::AlsaSourceNode> capture =
        eavp::detail::AlsaSourceNodeTestPeer::create(
            "mic0", eavp_test::make_alsa_config(), &metrics, &health,
            source.take_system()).take_value();
    std::unique_ptr<AudioChecksumSink> sink(new AudioChecksumSink());
    AudioChecksumSink* observed_sink = sink.get();
    ASSERT_TRUE(eavp::connect(capture->output(), observed_sink->input()).ok());

    eavp::MediaPipeline pipeline("alsa-live");
    ASSERT_TRUE(pipeline.add_node(std::move(capture)).ok());
    ASSERT_TRUE(pipeline.add_node(std::move(sink)).ok());
    ASSERT_TRUE(pipeline.connect("mic0", "audio-checksum").ok());
    ASSERT_TRUE(pipeline.start().ok());
    std::size_t turns = 0U;
    for (; turns < 2000U && observed_sink->frames() < 300U; ++turns) {
        ASSERT_TRUE(pipeline.tick().ok());
    }

    EXPECT_EQ(300U, observed_sink->frames());
    EXPECT_EQ(300U * 480U, observed_sink->samples());
    EXPECT_EQ(expected_checksum(), observed_sink->checksum());
    EXPECT_EQ(1U, observed_sink->discontinuities());
    EXPECT_TRUE(observed_sink->pts_steps_are_ten_ms());
    EXPECT_LT(turns, 2000U);
    EXPECT_EQ(1U, metrics.counter("alsa_capture.mic0.xruns").value());
    EXPECT_EQ(eavp::HealthStatus::kDegraded, health.aggregate());
    ASSERT_TRUE(pipeline.stop().ok());
    EXPECT_EQ(0, source.open_handles());
}

}  // namespace
