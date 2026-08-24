#include <gtest/gtest.h>

#include <cstdlib>
#include <ctime>
#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <string>

#include "eavp/management/health.hpp"
#include "eavp/management/metrics.hpp"
#include "eavp/media/pipeline.hpp"
#include "eavp/media/port.hpp"
#include "eavp/platform/linux/alsa_capture.hpp"

namespace {

struct DeviceTestConfig {
    DeviceTestConfig(const std::string& configured_device,
                     eavp::SampleFormat configured_sample_format,
                     int configured_sample_rate, int configured_channels,
                     int configured_samples_per_frame, int configured_frame_count,
                     int configured_timeout_seconds)
        : device(configured_device), sample_format(configured_sample_format),
          sample_rate(configured_sample_rate), channels(configured_channels),
          samples_per_frame(configured_samples_per_frame),
          frame_count(configured_frame_count),
          timeout_seconds(configured_timeout_seconds) {}

    std::string device;
    eavp::SampleFormat sample_format;
    int sample_rate;
    int channels;
    int samples_per_frame;
    int frame_count;
    int timeout_seconds;
};

std::string status_description(const eavp::Status& status) {
    std::ostringstream stream;
    stream << "code=" << static_cast<int>(status.code());
    if (!status.message().empty()) stream << ", message=" << status.message();
    if (!status.provider_id().empty()) stream << ", provider=" << status.provider_id();
    if (!status.operation().empty()) stream << ", operation=" << status.operation();
    if (status.has_native_code()) stream << ", native_code=" << status.native_code();
    return stream.str();
}

eavp::Result<std::string> string_env(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    if (value == NULL) return eavp::Result<std::string>(std::string(fallback));
    if (value[0] == '\0') {
        return eavp::Result<std::string>(eavp::Status(
            eavp::StatusCode::kInvalidArgument,
            std::string("环境变量 ") + name + " 不能为空"));
    }
    return eavp::Result<std::string>(std::string(value));
}

eavp::Result<int> positive_int_env(const char* name, int fallback, int minimum,
                                   int maximum) {
    const char* value = std::getenv(name);
    if (value == NULL) return eavp::Result<int>(fallback);
    if (value[0] == '\0') {
        return eavp::Result<int>(eavp::Status(
            eavp::StatusCode::kInvalidArgument,
            std::string("环境变量 ") + name + " 必须是十进制整数"));
    }

    int parsed = 0;
    for (const char* cursor = value; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return eavp::Result<int>(eavp::Status(
                eavp::StatusCode::kInvalidArgument,
                std::string("环境变量 ") + name + " 必须是十进制整数"));
        }
        const int digit = *cursor - '0';
        if (parsed > (maximum - digit) / 10) {
            return eavp::Result<int>(eavp::Status(
                eavp::StatusCode::kInvalidArgument,
                std::string("环境变量 ") + name + " 超出允许范围 [" +
                    std::to_string(minimum) + ", " + std::to_string(maximum) + "]"));
        }
        parsed = parsed * 10 + digit;
    }
    if (parsed < minimum || parsed > maximum) {
        return eavp::Result<int>(eavp::Status(
            eavp::StatusCode::kInvalidArgument,
            std::string("环境变量 ") + name + " 超出允许范围 [" +
                std::to_string(minimum) + ", " + std::to_string(maximum) + "]"));
    }
    return eavp::Result<int>(parsed);
}

eavp::Result<eavp::SampleFormat> sample_format_env() {
    const eavp::Result<std::string> value =
        string_env("EAVP_ALSA_SAMPLE_FORMAT", "s16le");
    if (!value.ok()) return eavp::Result<eavp::SampleFormat>(value.status());
    if (value.value() == "s16le") {
        return eavp::Result<eavp::SampleFormat>(
            eavp::SampleFormat::kSigned16LittleEndian);
    }
    if (value.value() == "s24le") {
        return eavp::Result<eavp::SampleFormat>(
            eavp::SampleFormat::kSigned24In32LittleEndian);
    }
    if (value.value() == "s32le") {
        return eavp::Result<eavp::SampleFormat>(
            eavp::SampleFormat::kSigned32LittleEndian);
    }
    if (value.value() == "f32le") {
        return eavp::Result<eavp::SampleFormat>(
            eavp::SampleFormat::kFloat32LittleEndian);
    }
    return eavp::Result<eavp::SampleFormat>(eavp::Status(
        eavp::StatusCode::kInvalidArgument,
        "环境变量 EAVP_ALSA_SAMPLE_FORMAT 只接受 s16le、s24le、s32le 或 f32le"));
}

eavp::Result<DeviceTestConfig> read_config() {
    const eavp::Result<std::string> device =
        string_env("EAVP_ALSA_DEVICE", "hw:Loopback,1,0");
    if (!device.ok()) return eavp::Result<DeviceTestConfig>(device.status());
    const eavp::Result<eavp::SampleFormat> sample_format = sample_format_env();
    if (!sample_format.ok()) return eavp::Result<DeviceTestConfig>(sample_format.status());
    const eavp::Result<int> sample_rate =
        positive_int_env("EAVP_ALSA_SAMPLE_RATE", 48000, 1, 768000);
    if (!sample_rate.ok()) return eavp::Result<DeviceTestConfig>(sample_rate.status());
    const eavp::Result<int> channels =
        positive_int_env("EAVP_ALSA_CHANNELS", 2, 1, 2);
    if (!channels.ok()) return eavp::Result<DeviceTestConfig>(channels.status());
    const eavp::Result<int> samples_per_frame =
        positive_int_env("EAVP_ALSA_SAMPLES_PER_FRAME", 480, 1, 48000);
    if (!samples_per_frame.ok()) {
        return eavp::Result<DeviceTestConfig>(samples_per_frame.status());
    }
    const eavp::Result<int> frame_count =
        positive_int_env("EAVP_ALSA_FRAME_COUNT", 300, 1, 100000);
    if (!frame_count.ok()) return eavp::Result<DeviceTestConfig>(frame_count.status());
    const eavp::Result<int> timeout_seconds =
        positive_int_env("EAVP_ALSA_TIMEOUT_SECONDS", 10, 1, 600);
    if (!timeout_seconds.ok()) {
        return eavp::Result<DeviceTestConfig>(timeout_seconds.status());
    }
    return eavp::Result<DeviceTestConfig>(DeviceTestConfig(
        device.value(), sample_format.value(), sample_rate.value(), channels.value(),
        samples_per_frame.value(), frame_count.value(), timeout_seconds.value()));
}

class DeviceAudioSink : public eavp::MediaNode {
public:
    explicit DeviceAudioSink(const DeviceTestConfig& config)
        : eavp::MediaNode("alsa-device-sink"),
          input_("alsa-device-input", 8U, eavp::OverflowPolicy::kBlock),
          config_(config), frames_(0), samples_(0), has_previous_pts_(false),
          previous_pts_(0) {}

    eavp::InputPort<eavp::AudioFrame>& input() { return input_; }
    int frames() const { return frames_; }
    std::uint64_t samples() const { return samples_; }

protected:
    eavp::Status on_tick() override {
        eavp::Result<std::shared_ptr<const eavp::AudioFrame> > received = input_.receive();
        if (!received.ok()) return received.status();
        const std::shared_ptr<const eavp::AudioFrame> frame = received.take_value();
        const eavp::AudioFormat& format = frame->format();
        if (format.sample_format() != config_.sample_format ||
            format.sample_rate() != config_.sample_rate ||
            format.channels() != config_.channels ||
            format.sample_layout() != eavp::AudioSampleLayout::kInterleaved ||
            format.memory_domain() != eavp::MemoryDomain::kCpu ||
            frame->samples_per_channel() != config_.samples_per_frame ||
            frame->time_base().numerator() != 1 ||
            frame->time_base().denominator() != 1000000) {
            return eavp::Status(eavp::StatusCode::kCorruptData,
                                "设备帧格式或时间基准与请求配置不一致");
        }
        if (frame->buffer().plane_count() != 1U) {
            return eavp::Status(eavp::StatusCode::kCorruptData,
                                "设备帧必须恰有一个交织 PCM plane");
        }
        const eavp::Result<eavp::PlaneLayout> layout = frame->buffer().plane_layout(0U);
        if (!layout.ok()) return layout.status();
        const std::size_t expected_bytes =
            static_cast<std::size_t>(config_.samples_per_frame) *
            format.bytes_per_pcm_frame();
        if (layout.value().size != expected_bytes) {
            return eavp::Status(eavp::StatusCode::kCorruptData,
                                "设备帧 PCM 字节数与请求配置不一致");
        }
        if (has_previous_pts_) {
            if (frame->pts() <= previous_pts_) {
                return eavp::Status(eavp::StatusCode::kCorruptData,
                                    "设备帧 PTS 不是单调递增");
            }
            if (!frame->discontinuity() && config_.sample_rate == 48000 &&
                config_.samples_per_frame == 480 && frame->pts() - previous_pts_ != 10000) {
                return eavp::Status(eavp::StatusCode::kCorruptData,
                                    "默认 48 kHz/480 设备帧的 PTS 间隔不是 10000 us");
            }
        }
        previous_pts_ = frame->pts();
        has_previous_pts_ = true;
        if (samples_ > std::numeric_limits<std::uint64_t>::max() -
                           static_cast<std::uint64_t>(frame->samples_per_channel())) {
            return eavp::Status(eavp::StatusCode::kCorruptData,
                                "设备测试采样计数溢出");
        }
        samples_ += static_cast<std::uint64_t>(frame->samples_per_channel());
        ++frames_;
        return eavp::Status::ok_status();
    }

private:
    eavp::InputPort<eavp::AudioFrame> input_;
    DeviceTestConfig config_;
    int frames_;
    std::uint64_t samples_;
    bool has_previous_pts_;
    std::int64_t previous_pts_;
};

bool deadline_reached(const struct timespec& started, int timeout_seconds) {
    struct timespec now;
    if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) return true;
    const std::int64_t elapsed_seconds = static_cast<std::int64_t>(now.tv_sec) -
        static_cast<std::int64_t>(started.tv_sec);
    const std::int64_t elapsed_nanoseconds = static_cast<std::int64_t>(now.tv_nsec) -
        static_cast<std::int64_t>(started.tv_nsec);
    return elapsed_seconds > timeout_seconds ||
        (elapsed_seconds == timeout_seconds && elapsed_nanoseconds >= 0);
}

void report_failure_and_cancel(eavp::MediaPipeline* pipeline,
                               const eavp::Status& status) {
    pipeline->cancel();
    ADD_FAILURE() << status_description(status);
}

TEST(AlsaCaptureDeviceTest, CapturesConfiguredFramesFromExistingProducer) {
    const eavp::Result<DeviceTestConfig> device_config = read_config();
    ASSERT_TRUE(device_config.ok()) << status_description(device_config.status());
    const DeviceTestConfig& config = device_config.value();

    const eavp::AudioChannelLayout channel_layout = config.channels == 1
        ? eavp::AudioChannelLayout::kMono : eavp::AudioChannelLayout::kStereo;
    const eavp::Result<eavp::AudioFormat> audio_format = eavp::AudioFormat::create(
        config.sample_format, config.sample_rate, channel_layout,
        eavp::AudioSampleLayout::kInterleaved, eavp::MemoryDomain::kCpu);
    ASSERT_TRUE(audio_format.ok()) << status_description(audio_format.status());
    const eavp::Result<eavp::AlsaCaptureConfig> capture_config =
        eavp::AlsaCaptureConfig::create(
            config.device, audio_format.value(), config.samples_per_frame,
            config.samples_per_frame, 4);
    ASSERT_TRUE(capture_config.ok()) << status_description(capture_config.status());

    eavp::MetricRegistry metrics;
    eavp::HealthManager health;
    eavp::Result<std::unique_ptr<eavp::AlsaSourceNode> > capture_result =
        eavp::AlsaSourceNode::create("alsa-device", capture_config.value(),
                                     &metrics, &health);
    ASSERT_TRUE(capture_result.ok()) << status_description(capture_result.status());
    std::unique_ptr<DeviceAudioSink> sink(new DeviceAudioSink(config));
    DeviceAudioSink* observed_sink = sink.get();
    ASSERT_TRUE(eavp::connect(capture_result.value()->output(), observed_sink->input()).ok());

    eavp::MediaPipeline pipeline("alsa-device-pipeline");
    ASSERT_TRUE(pipeline.add_node(capture_result.take_value()).ok());
    ASSERT_TRUE(pipeline.add_node(std::move(sink)).ok());
    ASSERT_TRUE(pipeline.connect("alsa-device", "alsa-device-sink").ok());
    const eavp::Status start = pipeline.start();
    ASSERT_TRUE(start.ok()) << "设备配置或打开失败: " << status_description(start);

    struct timespec started;
    ASSERT_EQ(0, ::clock_gettime(CLOCK_MONOTONIC, &started));
    while (observed_sink->frames() < config.frame_count &&
           !deadline_reached(started, config.timeout_seconds)) {
        const eavp::Status tick = pipeline.tick();
        if (!tick.ok() && tick.code() != eavp::StatusCode::kWouldBlock) {
            report_failure_and_cancel(&pipeline, tick);
            return;
        }
    }
    if (observed_sink->frames() != config.frame_count) {
        pipeline.cancel();
        ADD_FAILURE() << "环境超时：设备 " << config.device << " 在 "
                      << config.timeout_seconds << " 秒内仅采集到 "
                      << observed_sink->frames() << "/" << config.frame_count
                      << " 帧；请确认外部 Loopback producer、设备权限和精确格式配置。";
        return;
    }

    const std::uint64_t expected_samples =
        static_cast<std::uint64_t>(config.frame_count) *
        static_cast<std::uint64_t>(config.samples_per_frame);
    const std::uint64_t expected_bytes =
        expected_samples * static_cast<std::uint64_t>(audio_format.value().bytes_per_pcm_frame());
    const eavp::Result<std::uint64_t> frames_metric =
        metrics.counter("alsa_capture.alsa-device.frames");
    const eavp::Result<std::uint64_t> samples_metric =
        metrics.counter("alsa_capture.alsa-device.samples");
    const eavp::Result<std::uint64_t> bytes_metric =
        metrics.counter("alsa_capture.alsa-device.bytes");
    ASSERT_TRUE(frames_metric.ok()) << status_description(frames_metric.status());
    ASSERT_TRUE(samples_metric.ok()) << status_description(samples_metric.status());
    ASSERT_TRUE(bytes_metric.ok()) << status_description(bytes_metric.status());
    EXPECT_EQ(config.frame_count, observed_sink->frames());
    EXPECT_EQ(expected_samples, observed_sink->samples());
    EXPECT_EQ(static_cast<std::uint64_t>(config.frame_count), frames_metric.value());
    EXPECT_EQ(expected_samples, samples_metric.value());
    EXPECT_EQ(expected_bytes, bytes_metric.value());
    const eavp::Result<eavp::HealthComponent> component =
        health.component("alsa_capture/alsa-device");
    ASSERT_TRUE(component.ok()) << status_description(component.status());
    EXPECT_TRUE(component.value().status == eavp::HealthStatus::kOk ||
                component.value().status == eavp::HealthStatus::kDegraded);
    EXPECT_TRUE(health.aggregate() == eavp::HealthStatus::kOk ||
                health.aggregate() == eavp::HealthStatus::kDegraded);

    const eavp::Status stop = pipeline.stop();
    EXPECT_TRUE(stop.ok()) << status_description(stop);
}

}  // namespace
