#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <unistd.h>
#include <vector>

#include "eavp/management/health.hpp"
#include "eavp/management/metrics.hpp"
#include "eavp/media/pipeline.hpp"
#include "eavp/media/port.hpp"
#include "eavp/platform/linux/alsa_capture.hpp"
#include "eavp/platform/linux/platform_runtime.hpp"
#include "platform/linux/alsa_capture_internal.hpp"
#include "support/audio_test_utils.hpp"
#include "support/fake_alsa_api.hpp"

namespace {

const std::uint64_t kFnv1aOffsetBasis = 14695981039346656037ULL;
const std::uint64_t kFnv1aPrime = 1099511628211ULL;

class ScopedReadyPipe {
public:
    ScopedReadyPipe() : read_fd_(-1), write_fd_(-1) {
        int descriptors[2] = {-1, -1};
        if (::pipe(descriptors) != 0) throw std::runtime_error("pipe");
        read_fd_ = descriptors[0];
        write_fd_ = descriptors[1];
        if (!configure(read_fd_) || !configure(write_fd_)) {
            ::close(write_fd_);
            ::close(read_fd_);
            write_fd_ = -1;
            read_fd_ = -1;
            throw std::runtime_error("fcntl");
        }
    }

    ~ScopedReadyPipe() {
        if (write_fd_ >= 0) ::close(write_fd_);
        if (read_fd_ >= 0) ::close(read_fd_);
    }

    int read_fd() const { return read_fd_; }

    bool signal() {
        const std::uint8_t value = 1U;
        return ::write(write_fd_, &value, sizeof(value)) ==
               static_cast<ssize_t>(sizeof(value));
    }

private:
    static bool configure(int fd) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        const int descriptor_flags = ::fcntl(fd, F_GETFD, 0);
        return flags >= 0 && descriptor_flags >= 0 &&
               ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
               ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0;
    }

    int read_fd_;
    int write_fd_;

    ScopedReadyPipe(const ScopedReadyPipe&);
    ScopedReadyPipe& operator=(const ScopedReadyPipe&);
};

class RuntimeAlsaApi : public eavp_test::FakeAlsaApi {
public:
    RuntimeAlsaApi() : next_byte_(0U) {}

    snd_pcm_sframes_t pcm_readi(snd_pcm_t*, void* destination,
                                snd_pcm_uframes_t requested) override {
        if (pcm_read_results.empty()) return 0;
        const snd_pcm_sframes_t result = pcm_read_results.front();
        pcm_read_results.pop_front();
        if (result <= 0 || destination == NULL) return result;
        const std::size_t frames = std::min(
            static_cast<std::size_t>(result),
            static_cast<std::size_t>(requested));
        std::uint8_t* bytes = static_cast<std::uint8_t*>(destination);
        for (std::size_t index = 0U; index < frames * 4U; ++index) {
            bytes[index] = next_byte_++;
        }
        return result;
    }

private:
    std::uint8_t next_byte_;
};

class AudioChecksumSink : public eavp::MediaNode {
public:
    AudioChecksumSink()
        : eavp::MediaNode("audio-checksum"),
          input_("audio-input", 4U, eavp::OverflowPolicy::kBlock),
          frames_(0U), samples_(0U), checksum_(kFnv1aOffsetBasis) {}

    eavp::InputPort<eavp::AudioFrame>& input() { return input_; }

    bool wait_for_frames(std::size_t count,
                         const std::chrono::milliseconds& timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, timeout,
                                   [this, count]() { return frames_ >= count; });
    }

    std::size_t frames() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return frames_;
    }
    std::size_t samples() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return samples_;
    }
    std::uint64_t checksum() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return checksum_;
    }

protected:
    eavp::Status on_tick() override {
        eavp::Result<std::shared_ptr<const eavp::AudioFrame> > received =
            input_.receive();
        if (!received.ok()) return received.status();
        const std::shared_ptr<const eavp::AudioFrame> frame =
            received.take_value();
        eavp::Result<eavp::MappedRegion> mapped = frame->buffer().map_plane(
            0U, eavp::MapMode::kReadOnly);
        if (!mapped.ok()) return mapped.status();
        const eavp::MappedRegion bytes = mapped.take_value();

        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t index = 0U; index < bytes.size(); ++index) {
            checksum_ ^= static_cast<std::uint64_t>(bytes.data()[index]);
            checksum_ *= kFnv1aPrime;
        }
        samples_ += static_cast<std::size_t>(frame->samples_per_channel());
        ++frames_;
        condition_.notify_all();
        return eavp::Status::ok_status();
    }

private:
    eavp::InputPort<eavp::AudioFrame> input_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t frames_;
    std::size_t samples_;
    std::uint64_t checksum_;
};

TEST(AlsaCaptureRuntimeTest,
     RealPipeReadinessDrivesThreeHundredFramesWithoutDirectTicks) {
    ScopedReadyPipe ready_pipe;
    std::unique_ptr<RuntimeAlsaApi> fake(new RuntimeAlsaApi());
    RuntimeAlsaApi* observed_api = fake.get();
    const struct pollfd descriptor = {
        ready_pipe.read_fd(), static_cast<short>(POLLIN), 0};
    observed_api->poll_descriptor_values.push_back(descriptor);
    observed_api->poll_revents_value = POLLIN;
    for (std::size_t index = 0U; index < 300U; ++index) {
        observed_api->pcm_read_results.push_back(480);
    }

    eavp::MetricRegistry metrics;
    eavp::HealthManager health;
    std::unique_ptr<eavp::detail::AlsaSystem> system(
        new eavp::detail::AlsaSystem(
            std::unique_ptr<eavp::detail::AlsaApi>(fake.release())));
    eavp::Result<std::unique_ptr<eavp::AlsaSourceNode> > capture_result =
        eavp::detail::AlsaSourceNodeTestPeer::create(
            "mic0", eavp_test::make_alsa_config(), &metrics, &health,
            std::move(system));
    ASSERT_TRUE(capture_result.ok());
    std::unique_ptr<eavp::AlsaSourceNode> capture =
        capture_result.take_value();
    eavp::AlsaSourceNode* wait_source = capture.get();
    std::unique_ptr<AudioChecksumSink> sink(new AudioChecksumSink());
    AudioChecksumSink* observed_sink = sink.get();
    ASSERT_TRUE(eavp::connect(capture->output(), sink->input()).ok());

    eavp::MediaPipeline pipeline("alsa-runtime-live");
    ASSERT_TRUE(pipeline.add_node(std::move(capture)).ok());
    ASSERT_TRUE(pipeline.add_node(std::move(sink)).ok());
    ASSERT_TRUE(pipeline.connect("mic0", "audio-checksum").ok());
    const eavp::Result<eavp::LinuxPlatformRuntimeConfig> runtime_config =
        eavp::LinuxPlatformRuntimeConfig::create(1, 2000);
    ASSERT_TRUE(runtime_config.ok());
    eavp::Result<std::unique_ptr<eavp::LinuxPlatformRuntime> > runtime_result =
        eavp::LinuxPlatformRuntime::create(runtime_config.value(), &metrics);
    ASSERT_TRUE(runtime_result.ok());
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime =
        runtime_result.take_value();
    ASSERT_TRUE(runtime->register_pipeline(
        &pipeline,
        std::vector<eavp::LinuxWaitSource*>(1U, wait_source)).ok());
    ASSERT_TRUE(runtime->start().ok());

    const bool signaled = ready_pipe.signal();
    const bool completed = signaled && observed_sink->wait_for_frames(
        300U, std::chrono::milliseconds(5000));
    const eavp::Status stop_status = runtime->stop();

    EXPECT_TRUE(signaled);
    EXPECT_TRUE(completed);
    EXPECT_TRUE(stop_status.ok());
    EXPECT_EQ(300U, observed_sink->frames());
    EXPECT_EQ(300U * 480U, observed_sink->samples());
    EXPECT_NE(kFnv1aOffsetBasis, observed_sink->checksum());
    EXPECT_GT(observed_api->poll_revents_calls, 0);
    EXPECT_EQ(0, observed_api->successful_open_count - observed_api->close_count);
}

}  // namespace
