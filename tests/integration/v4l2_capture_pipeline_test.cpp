#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <linux/videodev2.h>
#include <memory>
#include <vector>

#include "eavp/management/health.hpp"
#include "eavp/management/metrics.hpp"
#include "eavp/media/pipeline.hpp"
#include "eavp/media/port.hpp"
#include "platform/linux/platform_runtime_internal.hpp"
#include "platform/linux/v4l2_capture_internal.hpp"
#include "support/fake_linux_runtime_api.hpp"
#include "support/fake_v4l2_api.hpp"
#include "support/v4l2_test_utils.hpp"

namespace {

const std::size_t kFrameBytes = 192U;
const std::uint64_t kFnv1aOffsetBasis = 14695981039346656037ULL;
const std::uint64_t kFnv1aPrime = 1099511628211ULL;

std::uint64_t checksum_for(std::size_t frame_count) {
    std::uint64_t checksum = kFnv1aOffsetBasis;
    for (std::size_t frame = 0U; frame < frame_count; ++frame) {
        for (std::size_t offset = 0U; offset < kFrameBytes; ++offset) {
            checksum ^= eavp_test::FakeV4L2Api::runtime_payload_byte(
                static_cast<std::uint32_t>(frame), offset);
            checksum *= kFnv1aPrime;
        }
    }
    return checksum;
}

class CpuFrameAllocator : public eavp::detail::V4L2FrameAllocator {
public:
    eavp::Result<eavp::Buffer> allocate(
        std::size_t capacity,
        const std::vector<eavp::PlaneLayout>& planes) override {
        return eavp::Buffer::allocate(capacity, planes);
    }
};

class RuntimeV4L2Observer : public eavp::detail::V4L2Observer {
public:
    RuntimeV4L2Observer(eavp::MetricRegistry* metrics,
                        eavp::HealthManager* health)
        : metrics_(metrics), health_(health) {}

    eavp::Status on_captured(std::size_t copied_bytes) override {
        eavp::Status status = eavp::Status::ok_status();
        if (metrics_ != NULL) {
            status = metrics_->increment_counter("v4l2.frames.captured");
            if (!status.ok()) return status;
            status = metrics_->increment_counter(
                "v4l2.bytes.copied", static_cast<std::uint64_t>(copied_bytes));
            if (!status.ok()) return status;
        }
        return health_ == NULL
            ? eavp::Status::ok_status()
            : health_->report("v4l2.capture", eavp::HealthStatus::kOk,
                              "Runtime V4L2 捕获正常");
    }

    eavp::Status on_would_block() override {
        return metrics_ == NULL
            ? eavp::Status::ok_status()
            : metrics_->increment_counter("v4l2.dequeue.would_block");
    }

    eavp::Status on_pending(bool pending) override {
        return metrics_ == NULL
            ? eavp::Status::ok_status()
            : metrics_->set_gauge("v4l2.pending_frame", pending ? 1.0 : 0.0);
    }

    eavp::Status on_dropped_on_stop() override {
        return metrics_ == NULL
            ? eavp::Status::ok_status()
            : metrics_->increment_counter("v4l2.frames.dropped_on_stop");
    }

    eavp::Status on_sequence_gap(std::uint32_t missing_frames) override {
        return metrics_ == NULL
            ? eavp::Status::ok_status()
            : metrics_->increment_counter(
                  "v4l2.sequence.gaps",
                  static_cast<std::uint64_t>(missing_frames));
    }

    eavp::Status on_fatal(const eavp::Status&) override {
        return health_ == NULL
            ? eavp::Status::ok_status()
            : health_->report("v4l2.capture", eavp::HealthStatus::kError,
                              "Runtime V4L2 捕获失败");
    }

private:
    eavp::MetricRegistry* metrics_;
    eavp::HealthManager* health_;
};

class V4L2RuntimePipeline {
public:
    explicit V4L2RuntimePipeline(std::size_t frame_count)
        : metrics(), health(), trace(new eavp_test::FakeV4L2Trace()),
          capture_observer(&metrics, &health), runtime_api(NULL),
          pipeline("v4l2-runtime-live"), sink(NULL), wait_source(), runtime() {
        std::unique_ptr<eavp_test::FakeLinuxRuntimeApi> fake_runtime(
            new eavp_test::FakeLinuxRuntimeApi());
        runtime_api = fake_runtime.get();
        runtime_api->event_fd_result = 42;
        runtime_api->enable_blocking_wait();

        std::unique_ptr<eavp_test::FakeV4L2Api> fake_v4l2(
            new eavp_test::FakeV4L2Api(trace));
        fake_v4l2->set_format(V4L2_PIX_FMT_YUV420, 16U, 8U, 16U,
                              static_cast<std::uint32_t>(kFrameBytes));
        fake_v4l2->script_runtime_frames(frame_count);
        fake_v4l2->set_runtime_ready_callback([this]() {
            runtime_api->queue_events(
                std::vector<eavp_test::FakeLinuxRuntimeApi::ReadyEvent>(
                    1U, eavp_test::FakeLinuxRuntimeApi::ReadyEvent(
                            41, EPOLLIN)));
        });

        std::unique_ptr<eavp::detail::V4L2System> system(
            new eavp::detail::V4L2System(
                std::unique_ptr<eavp::detail::V4L2Api>(fake_v4l2.release())));
        const eavp::V4L2CaptureConfig capture_config =
            eavp::V4L2CaptureConfig::create(
                "/dev/video-test", eavp::PixelFormat::kYuv420p,
                16, 8, 30, 1, 3U).take_value();
        std::unique_ptr<eavp::V4L2SourceNode> capture =
            eavp::detail::V4L2SourceNodeTestPeer::create(
                "camera0", capture_config, &metrics, std::move(system),
                std::unique_ptr<eavp::detail::V4L2FrameAllocator>(
                    new CpuFrameAllocator()), &capture_observer).take_value();
        wait_source.reset(
            new eavp_test::RuntimeV4L2WaitSource(capture.get()));
        std::unique_ptr<eavp_test::FrameChecksumSink> owned_sink(
            new eavp_test::FrameChecksumSink());
        sink = owned_sink.get();
        EXPECT_TRUE(eavp::connect(capture->output(), sink->input()).ok());
        EXPECT_TRUE(pipeline.add_node(std::move(capture)).ok());
        EXPECT_TRUE(pipeline.add_node(std::move(owned_sink)).ok());
        EXPECT_TRUE(pipeline.connect("camera0", "frame-checksum").ok());

        const eavp::LinuxPlatformRuntimeConfig runtime_config =
            eavp::LinuxPlatformRuntimeConfig::create(1, 2000).take_value();
        runtime = eavp::detail::LinuxPlatformRuntimeTestPeer::create(
            runtime_config,
            std::unique_ptr<eavp::detail::LinuxRuntimeApi>(
                fake_runtime.release())).take_value();
        EXPECT_TRUE(runtime->register_pipeline(
            &pipeline,
            std::vector<eavp::LinuxWaitSource*>(
                1U, wait_source.get())).ok());
    }

    ~V4L2RuntimePipeline() {
        if (runtime) runtime->stop();
    }

    bool wait_until_sink_frames(std::size_t count, int timeout_ms) {
        return sink->wait_for_frames(count, timeout_ms);
    }

    int open_handles() const {
        const int v4l2_handles = static_cast<int>(trace->open_calls) -
                                 static_cast<int>(trace->close_calls);
        const int runtime_handles = runtime_api->epoll_create_count +
            runtime_api->create_event_fd_count -
            static_cast<int>(runtime_api->closed_fds.size());
        return v4l2_handles + runtime_handles;
    }

    eavp::MetricRegistry metrics;
    eavp::HealthManager health;
    std::shared_ptr<eavp_test::FakeV4L2Trace> trace;
    RuntimeV4L2Observer capture_observer;
    eavp_test::FakeLinuxRuntimeApi* runtime_api;
    eavp::MediaPipeline pipeline;
    eavp_test::FrameChecksumSink* sink;
    std::unique_ptr<eavp_test::RuntimeV4L2WaitSource> wait_source;
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime;
};

TEST(V4L2CapturePipelineTest, CapturesExactlyThreeHundredFramesViaRuntime) {
    V4L2RuntimePipeline fixture(300U);
    const eavp::Status start_status = fixture.runtime->start();
    ASSERT_TRUE(start_status.ok())
        << "code=" << static_cast<int>(start_status.code())
        << " " << start_status.message();
    ASSERT_TRUE(fixture.wait_until_sink_frames(300U, 5000))
        << "actual frames=" << fixture.sink->frame_count();
    const eavp::Status stop_status = fixture.runtime->stop();
    ASSERT_TRUE(stop_status.ok());

    EXPECT_EQ(300U, fixture.sink->frame_count());
    EXPECT_TRUE(fixture.sink->pts_are_monotonic());
    EXPECT_FALSE(fixture.sink->has_duplicate_sequence());
    EXPECT_TRUE(fixture.sink->plane_layouts_are_consistent());
    const std::vector<eavp::PlaneLayout> layouts =
        fixture.sink->plane_layouts();
    ASSERT_EQ(3U, layouts.size());
    EXPECT_EQ(0U, layouts[0].offset);
    EXPECT_EQ(128U, layouts[0].size);
    EXPECT_EQ(16U, layouts[0].stride);
    EXPECT_EQ(128U, layouts[1].offset);
    EXPECT_EQ(32U, layouts[1].size);
    EXPECT_EQ(8U, layouts[1].stride);
    EXPECT_EQ(160U, layouts[2].offset);
    EXPECT_EQ(32U, layouts[2].size);
    EXPECT_EQ(8U, layouts[2].stride);
    EXPECT_EQ(0U, fixture.sink->dropped_count());
    EXPECT_EQ(checksum_for(300U), fixture.sink->checksum());
    EXPECT_EQ(300U,
              fixture.metrics.counter("v4l2.frames.captured").value());
    EXPECT_EQ(eavp::HealthStatus::kOk,
              fixture.health.component("v4l2.capture").value().status);
    EXPECT_EQ(300U, fixture.trace->dequeued_buffer_types.size());
    EXPECT_EQ(0, fixture.open_handles());
}

}  // namespace
