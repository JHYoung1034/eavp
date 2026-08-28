#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

#include "eavp/management/health.hpp"
#include "eavp/management/metrics.hpp"
#include "eavp/media/pipeline.hpp"
#include "eavp/media/port.hpp"
#include "eavp/platform/linux/platform_runtime.hpp"
#include "eavp/platform/linux/v4l2_capture.hpp"

namespace {

const std::uint64_t kFnv1aOffsetBasis = 14695981039346656037ULL;
const std::uint64_t kFnv1aPrime = 1099511628211ULL;

struct DeviceTestConfig {
    DeviceTestConfig(const std::string& configured_device,
                     eavp::PixelFormat configured_pixel_format,
                     std::uint32_t configured_v4l2_pixel_format,
                     int configured_width, int configured_height,
                     int configured_frame_rate_numerator,
                     int configured_frame_rate_denominator,
                     int configured_frame_count,
                     int configured_timeout_seconds,
                     int configured_buffer_count)
        : device(configured_device), pixel_format(configured_pixel_format),
          v4l2_pixel_format(configured_v4l2_pixel_format),
          width(configured_width), height(configured_height),
          frame_rate_numerator(configured_frame_rate_numerator),
          frame_rate_denominator(configured_frame_rate_denominator),
          frame_count(configured_frame_count),
          timeout_seconds(configured_timeout_seconds),
          buffer_count(configured_buffer_count) {}

    std::string device;
    eavp::PixelFormat pixel_format;
    std::uint32_t v4l2_pixel_format;
    int width;
    int height;
    int frame_rate_numerator;
    int frame_rate_denominator;
    int frame_count;
    int timeout_seconds;
    int buffer_count;
};

class ScopedFd {
public:
    explicit ScopedFd(int fd) : fd_(fd) {}
    ~ScopedFd() {
        if (fd_ >= 0) ::close(fd_);
    }

    int get() const { return fd_; }

private:
    ScopedFd(const ScopedFd&);
    ScopedFd& operator=(const ScopedFd&);

    int fd_;
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

std::string fourcc_string(std::uint32_t value) {
    std::string text;
    text.push_back(static_cast<char>(value & 0xffU));
    text.push_back(static_cast<char>((value >> 8U) & 0xffU));
    text.push_back(static_cast<char>((value >> 16U) & 0xffU));
    text.push_back(static_cast<char>((value >> 24U) & 0xffU));
    return text;
}

eavp::Status errno_status(eavp::StatusCode code, const std::string& message,
                          const char* operation, int native_code) {
    return eavp::Status(code, message + ": " + std::strerror(native_code),
                        "v4l2_device_test", operation, native_code);
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

eavp::Result<int> positive_int_env(const char* name, int fallback,
                                   int minimum, int maximum) {
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
                std::string("环境变量 ") + name + " 超出允许范围"));
        }
        parsed = parsed * 10 + digit;
    }
    if (parsed < minimum || parsed > maximum) {
        return eavp::Result<int>(eavp::Status(
            eavp::StatusCode::kInvalidArgument,
            std::string("环境变量 ") + name + " 超出允许范围"));
    }
    return eavp::Result<int>(parsed);
}

eavp::Result<DeviceTestConfig> read_config() {
    const eavp::Result<std::string> device =
        string_env("EAVP_V4L2_DEVICE", "/dev/video10");
    if (!device.ok()) return eavp::Result<DeviceTestConfig>(device.status());

    const eavp::Result<std::string> pixel_format =
        string_env("EAVP_V4L2_PIXEL_FORMAT", "yuv420p");
    if (!pixel_format.ok()) {
        return eavp::Result<DeviceTestConfig>(pixel_format.status());
    }
    eavp::PixelFormat format = eavp::PixelFormat::kUnknown;
    std::uint32_t v4l2_format = 0U;
    if (pixel_format.value() == "yuv420p") {
        format = eavp::PixelFormat::kYuv420p;
        v4l2_format = V4L2_PIX_FMT_YUV420;
    } else if (pixel_format.value() == "nv12") {
        format = eavp::PixelFormat::kNv12;
        v4l2_format = V4L2_PIX_FMT_NV12;
    } else if (pixel_format.value() == "yuyv422") {
        format = eavp::PixelFormat::kYuyv422;
        v4l2_format = V4L2_PIX_FMT_YUYV;
    } else {
        return eavp::Result<DeviceTestConfig>(eavp::Status(
            eavp::StatusCode::kInvalidArgument,
            "环境变量 EAVP_V4L2_PIXEL_FORMAT 只接受 yuv420p、nv12 或 yuyv422"));
    }

    const eavp::Result<int> width =
        positive_int_env("EAVP_V4L2_WIDTH", 1920, 2, 32768);
    if (!width.ok()) return eavp::Result<DeviceTestConfig>(width.status());
    const eavp::Result<int> height =
        positive_int_env("EAVP_V4L2_HEIGHT", 1080, 2, 32768);
    if (!height.ok()) return eavp::Result<DeviceTestConfig>(height.status());
    const eavp::Result<int> fps_numerator =
        positive_int_env("EAVP_V4L2_FPS_NUMERATOR", 30, 1, 1000);
    if (!fps_numerator.ok()) {
        return eavp::Result<DeviceTestConfig>(fps_numerator.status());
    }
    const eavp::Result<int> fps_denominator =
        positive_int_env("EAVP_V4L2_FPS_DENOMINATOR", 1, 1, 1000);
    if (!fps_denominator.ok()) {
        return eavp::Result<DeviceTestConfig>(fps_denominator.status());
    }
    const eavp::Result<int> frame_count =
        positive_int_env("EAVP_V4L2_FRAME_COUNT", 300, 1, 100000);
    if (!frame_count.ok()) {
        return eavp::Result<DeviceTestConfig>(frame_count.status());
    }
    const eavp::Result<int> timeout_seconds =
        positive_int_env("EAVP_V4L2_TIMEOUT_SECONDS", 20, 1, 600);
    if (!timeout_seconds.ok()) {
        return eavp::Result<DeviceTestConfig>(timeout_seconds.status());
    }
    const eavp::Result<int> buffer_count =
        positive_int_env("EAVP_V4L2_BUFFER_COUNT", 4, 2, 1024);
    if (!buffer_count.ok()) {
        return eavp::Result<DeviceTestConfig>(buffer_count.status());
    }

    return eavp::Result<DeviceTestConfig>(DeviceTestConfig(
        device.value(), format, v4l2_format, width.value(), height.value(),
        fps_numerator.value(), fps_denominator.value(), frame_count.value(),
        timeout_seconds.value(), buffer_count.value()));
}

eavp::Status preflight_device(const DeviceTestConfig& config) {
    if (::access(config.device.c_str(), R_OK | W_OK) != 0) {
        return errno_status(errno == ENOENT ? eavp::StatusCode::kNotFound
                                            : eavp::StatusCode::kIoError,
                            "无法访问 V4L2 设备 " + config.device,
                            "access", errno);
    }

    ScopedFd fd(::open(config.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
    if (fd.get() < 0) {
        return errno_status(errno == ENOENT ? eavp::StatusCode::kNotFound
                                            : eavp::StatusCode::kIoError,
                            "无法打开 V4L2 设备 " + config.device,
                            "open", errno);
    }

    struct v4l2_capability capability;
    std::memset(&capability, 0, sizeof(capability));
    if (::ioctl(fd.get(), VIDIOC_QUERYCAP, &capability) != 0) {
        return errno_status(eavp::StatusCode::kIoError,
                            "无法查询 V4L2 capability", "VIDIOC_QUERYCAP",
                            errno);
    }
    std::uint32_t device_caps = capability.capabilities;
#ifdef V4L2_CAP_DEVICE_CAPS
    if ((capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U) {
        device_caps = capability.device_caps;
    }
#endif
    if ((device_caps & V4L2_CAP_VIDEO_CAPTURE) == 0U ||
        (device_caps & V4L2_CAP_STREAMING) == 0U) {
        return eavp::Status(
            eavp::StatusCode::kCapabilityMismatch,
            "V4L2 设备必须同时支持 VIDEO_CAPTURE 与 STREAMING");
    }

    struct v4l2_format current_format;
    std::memset(&current_format, 0, sizeof(current_format));
    current_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (::ioctl(fd.get(), VIDIOC_G_FMT, &current_format) != 0) {
        return errno_status(eavp::StatusCode::kIoError,
                            "无法读取 V4L2 当前格式", "VIDIOC_G_FMT", errno);
    }
    if (current_format.fmt.pix.pixelformat != config.v4l2_pixel_format ||
        current_format.fmt.pix.width != static_cast<std::uint32_t>(config.width) ||
        current_format.fmt.pix.height != static_cast<std::uint32_t>(config.height)) {
        std::ostringstream message;
        message << "V4L2 当前格式不匹配，期望 "
                << fourcc_string(config.v4l2_pixel_format) << " "
                << config.width << "x" << config.height << "，实际 "
                << fourcc_string(current_format.fmt.pix.pixelformat) << " "
                << current_format.fmt.pix.width << "x"
                << current_format.fmt.pix.height;
        return eavp::Status(eavp::StatusCode::kCapabilityMismatch,
                            message.str());
    }
    return eavp::Status::ok_status();
}

int count_open_device_fds(const std::string& device) {
    DIR* directory = ::opendir("/proc/self/fd");
    if (directory == NULL) return -1;

    int count = 0;
    struct dirent* entry = NULL;
    while ((entry = ::readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        std::string fd_path("/proc/self/fd/");
        fd_path += entry->d_name;
        char target[PATH_MAX];
        const ssize_t size = ::readlink(fd_path.c_str(), target, sizeof(target) - 1U);
        if (size < 0) continue;
        target[size] = '\0';
        if (device == target) ++count;
    }
    ::closedir(directory);
    return count;
}

class DeviceVideoSink : public eavp::MediaNode {
public:
    DeviceVideoSink(const DeviceTestConfig& config, eavp::HealthManager* health)
        : eavp::MediaNode("v4l2-device-sink"),
          input_("v4l2-device-input", 4U, eavp::OverflowPolicy::kDropOldest),
          config_(config), health_(health), frames_(0U),
          checksum_(kFnv1aOffsetBasis), bytes_(0U), has_previous_pts_(false),
          previous_pts_(0), pts_monotonic_(true), layouts_(),
          layouts_consistent_(true) {}

    eavp::InputPort<eavp::VideoFrame>& input() { return input_; }
    bool wait_for_frames(std::size_t count, int timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(
            lock, std::chrono::milliseconds(timeout_ms),
            [this, count]() { return frames_ >= count; });
    }
    std::size_t frames() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return frames_;
    }
    std::uint64_t checksum() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return checksum_;
    }
    std::uint64_t bytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return bytes_;
    }
    bool pts_monotonic() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pts_monotonic_;
    }
    bool layouts_consistent() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return layouts_consistent_;
    }
    std::vector<eavp::PlaneLayout> layouts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return layouts_;
    }
    std::size_t dropped_count() const { return input_.dropped_count(); }

protected:
    eavp::Status on_tick() override {
        eavp::Result<std::shared_ptr<const eavp::VideoFrame> > received =
            input_.receive();
        if (!received.ok()) return received.status();
        const std::shared_ptr<const eavp::VideoFrame> frame = received.take_value();
        const eavp::VideoFormat& format = frame->format();
        if (format.pixel_format() != config_.pixel_format ||
            format.width() != config_.width || format.height() != config_.height ||
            format.memory_domain() != eavp::MemoryDomain::kCpu ||
            frame->time_base().numerator() != 1 ||
            frame->time_base().denominator() != 1000000) {
            return eavp::Status(eavp::StatusCode::kCorruptData,
                                "V4L2 设备帧格式或时间基准与请求不一致");
        }
        if (frame->buffer().plane_count() != format.planes().size() ||
            frame->buffer().plane_count() == 0U) {
            return eavp::Status(eavp::StatusCode::kCorruptData,
                                "V4L2 设备帧 plane 数量不一致");
        }
        std::vector<eavp::PlaneLayout> current_layouts;
        std::uint64_t frame_checksum = kFnv1aOffsetBasis;
        std::uint64_t frame_bytes = 0U;
        for (std::size_t plane = 0U; plane < frame->buffer().plane_count(); ++plane) {
            eavp::Result<eavp::PlaneLayout> layout =
                frame->buffer().plane_layout(plane);
            if (!layout.ok()) return layout.status();
            current_layouts.push_back(layout.value());
            eavp::Result<eavp::MappedRegion> mapped =
                frame->buffer().map_plane(plane, eavp::MapMode::kReadOnly);
            if (!mapped.ok()) return mapped.status();
            eavp::MappedRegion region = mapped.take_value();
            frame_bytes += static_cast<std::uint64_t>(region.size());
            for (std::size_t index = 0U; index < region.size(); ++index) {
                frame_checksum ^= static_cast<std::uint64_t>(region.data()[index]);
                frame_checksum *= kFnv1aPrime;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (has_previous_pts_ && frame->pts() < previous_pts_) {
                pts_monotonic_ = false;
                return eavp::Status(eavp::StatusCode::kCorruptData,
                                    "V4L2 设备帧 PTS 发生回退");
            }
            has_previous_pts_ = true;
            previous_pts_ = frame->pts();
            bytes_ += frame_bytes;
            checksum_ ^= frame_checksum;
            checksum_ *= kFnv1aPrime;
            remember_layouts(current_layouts);
            ++frames_;
        }
        condition_.notify_all();
        return health_ == NULL
            ? eavp::Status::ok_status()
            : health_->report("v4l2_device/pipeline", eavp::HealthStatus::kOk,
                              "真实 V4L2 采集正常");
    }

private:
    void remember_layouts(const std::vector<eavp::PlaneLayout>& layouts) {
        if (layouts_.empty()) {
            layouts_ = layouts;
            return;
        }
        if (layouts_.size() != layouts.size()) {
            layouts_consistent_ = false;
            return;
        }
        for (std::size_t index = 0U; index < layouts.size(); ++index) {
            if (layouts_[index].offset != layouts[index].offset ||
                layouts_[index].size != layouts[index].size ||
                layouts_[index].stride != layouts[index].stride) {
                layouts_consistent_ = false;
            }
        }
    }

    eavp::InputPort<eavp::VideoFrame> input_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    DeviceTestConfig config_;
    eavp::HealthManager* health_;
    std::size_t frames_;
    std::uint64_t checksum_;
    std::uint64_t bytes_;
    bool has_previous_pts_;
    std::int64_t previous_pts_;
    bool pts_monotonic_;
    std::vector<eavp::PlaneLayout> layouts_;
    bool layouts_consistent_;
};

TEST(V4L2CaptureDeviceTest, PreflightsConfiguredDeviceCapabilities) {
    const eavp::Result<DeviceTestConfig> config = read_config();
    ASSERT_TRUE(config.ok()) << status_description(config.status());
    const eavp::Status preflight = preflight_device(config.value());
    ASSERT_TRUE(preflight.ok()) << status_description(preflight);
}

TEST(V4L2CaptureDeviceTest, CapturesConfiguredFramesViaRuntime) {
    const eavp::Result<DeviceTestConfig> config_result = read_config();
    ASSERT_TRUE(config_result.ok()) << status_description(config_result.status());
    const DeviceTestConfig& config = config_result.value();
    const eavp::Status preflight = preflight_device(config);
    ASSERT_TRUE(preflight.ok()) << status_description(preflight);

    const eavp::Result<eavp::V4L2CaptureConfig> capture_config =
        eavp::V4L2CaptureConfig::create(
            config.device, config.pixel_format, config.width, config.height,
            config.frame_rate_numerator, config.frame_rate_denominator,
            static_cast<std::size_t>(config.buffer_count));
    ASSERT_TRUE(capture_config.ok()) << status_description(capture_config.status());

    eavp::MetricRegistry metrics;
    eavp::HealthManager health;
    eavp::Result<std::unique_ptr<eavp::V4L2SourceNode> > capture_result =
        eavp::V4L2SourceNode::create("v4l2-device", capture_config.value(), &metrics);
    ASSERT_TRUE(capture_result.ok()) << status_description(capture_result.status());
    std::unique_ptr<eavp::V4L2SourceNode> capture = capture_result.take_value();
    eavp::V4L2SourceNode* wait_source = capture.get();

    std::unique_ptr<DeviceVideoSink> sink(new DeviceVideoSink(config, &health));
    DeviceVideoSink* observed_sink = sink.get();
    const eavp::Status port_connect =
        eavp::connect(capture->output(), observed_sink->input());
    ASSERT_TRUE(port_connect.ok()) << status_description(port_connect);

    eavp::MediaPipeline pipeline("v4l2-device-pipeline");
    const eavp::Status add_capture = pipeline.add_node(std::move(capture));
    ASSERT_TRUE(add_capture.ok()) << status_description(add_capture);
    const eavp::Status add_sink = pipeline.add_node(std::move(sink));
    ASSERT_TRUE(add_sink.ok()) << status_description(add_sink);
    const eavp::Status pipeline_connect =
        pipeline.connect("v4l2-device", "v4l2-device-sink");
    ASSERT_TRUE(pipeline_connect.ok()) << status_description(pipeline_connect);

    const eavp::Result<eavp::LinuxPlatformRuntimeConfig> runtime_config =
        eavp::LinuxPlatformRuntimeConfig::create(1, 5000);
    ASSERT_TRUE(runtime_config.ok()) << status_description(runtime_config.status());
    eavp::Result<std::unique_ptr<eavp::LinuxPlatformRuntime> > runtime_result =
        eavp::LinuxPlatformRuntime::create(runtime_config.value(), &metrics);
    ASSERT_TRUE(runtime_result.ok()) << status_description(runtime_result.status());
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime =
        runtime_result.take_value();
    const eavp::Status registered = runtime->register_pipeline(
        &pipeline,
        std::vector<eavp::LinuxWaitSource*>(1U, wait_source));
    ASSERT_TRUE(registered.ok()) << status_description(registered);

    const eavp::Status started = runtime->start();
    ASSERT_TRUE(started.ok()) << "设备配置或启动失败: "
                              << status_description(started);

    const bool completed = observed_sink->wait_for_frames(
        static_cast<std::size_t>(config.frame_count),
        config.timeout_seconds * 1000);
    const eavp::PlatformRuntimeState runtime_state = runtime->state();
    if (!completed || runtime_state == eavp::PlatformRuntimeState::kError) {
        const eavp::Status current_failure = runtime->last_failure();
        const eavp::Status timeout(
            eavp::StatusCode::kTimeout,
            "设备 " + config.device + " 在 " +
                std::to_string(config.timeout_seconds) + " 秒内仅采集到 " +
                std::to_string(static_cast<unsigned long long>(
                    observed_sink->frames())) + "/" +
                std::to_string(config.frame_count) + " 帧");
        const eavp::Status stopped = runtime->stop();
        ADD_FAILURE() << (runtime_state == eavp::PlatformRuntimeState::kError
                              ? status_description(current_failure)
                              : status_description(timeout))
                      << "; stop=" << status_description(stopped);
        return;
    }

    const eavp::Status stopped = runtime->stop();
    EXPECT_TRUE(stopped.ok()) << status_description(stopped);
    EXPECT_EQ(eavp::PlatformRuntimeState::kStopped, runtime->state());
    EXPECT_EQ(eavp::PipelineState::kStopped, pipeline.state());
    EXPECT_EQ(static_cast<std::size_t>(config.frame_count), observed_sink->frames());
    EXPECT_TRUE(observed_sink->pts_monotonic());
    EXPECT_TRUE(observed_sink->layouts_consistent());
    EXPECT_FALSE(observed_sink->layouts().empty());
    EXPECT_EQ(0U, observed_sink->dropped_count());
    EXPECT_NE(0U, observed_sink->bytes());
    (void)observed_sink->checksum();

    const eavp::Result<std::uint64_t> frames_metric =
        metrics.counter("v4l2.frames.captured");
    ASSERT_TRUE(frames_metric.ok()) << status_description(frames_metric.status());
    EXPECT_EQ(static_cast<std::uint64_t>(config.frame_count),
              frames_metric.value());
    const eavp::Result<eavp::HealthComponent> component =
        health.component("v4l2_device/pipeline");
    ASSERT_TRUE(component.ok()) << status_description(component.status());
    EXPECT_EQ(eavp::HealthStatus::kOk, component.value().status);
    EXPECT_EQ(eavp::HealthStatus::kOk, health.aggregate());

    const int open_device_fds = count_open_device_fds(config.device);
    ASSERT_GE(open_device_fds, 0);
    EXPECT_EQ(0, open_device_fds);
}

}  // namespace
