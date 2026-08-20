#include <gtest/gtest.h>

#include "eavp/platform/reference_media_platform.hpp"

namespace {

TEST(ReferenceMediaPlatformTest,
     ProcessesOneHundredFramesAndPublishesSelection) {
    eavp::ReferenceMediaPlatform platform;
    ASSERT_TRUE(platform.initialize().ok());
    ASSERT_TRUE(platform.dispatch(eavp::StartPipelineCommand(
        "start-ref", "integration", "reference0")).ok());
    ASSERT_TRUE(platform.reconcile_once().ok());
    ASSERT_TRUE(platform.tick(100U).ok());

    eavp::Result<eavp::StateSnapshot> queried =
        platform.query(eavp::PipelineStateQuery("reference0"));
    ASSERT_TRUE(queried.ok());
    const eavp::StateSnapshot snapshot = queried.take_value();
    EXPECT_EQ("running", snapshot.get("/pipelines/reference0/state")
                             .value().as_string().value());
    EXPECT_EQ("reference", snapshot.get("/pipelines/reference0/provider")
                               .value().as_string().value());
    EXPECT_EQ("rgb24", snapshot.get("/pipelines/reference0/pixel_format")
                           .value().as_string().value());
    EXPECT_EQ("cpu", snapshot.get("/pipelines/reference0/memory_domain")
                         .value().as_string().value());
    EXPECT_EQ(100U,
              platform.metrics().counter("media.frames.encoded").value());
    EXPECT_DOUBLE_EQ(
        0.0, platform.metrics().gauge("pipeline.queue.depth").value());
    EXPECT_EQ(eavp::HealthStatus::kOk, platform.health().aggregate());
}

TEST(ReferenceMediaPlatformTest,
     DeviceLossPublishesStructuredErrorAndExplicitResetRebuildsPipeline) {
    eavp::ReferenceBackendOptions options;
    options.device_lost_after_submissions = 2U;
    eavp::ReferenceMediaPlatform platform(options);
    ASSERT_TRUE(platform.initialize().ok());
    ASSERT_TRUE(platform.dispatch(eavp::StartPipelineCommand(
        "start-failing", "integration", "reference0")).ok());
    ASSERT_TRUE(platform.reconcile_once().ok());

    const eavp::Status failure = platform.tick(3U);
    EXPECT_EQ(eavp::StatusCode::kDeviceLost, failure.code());
    eavp::Result<eavp::StateSnapshot> queried =
        platform.query(eavp::PipelineStateQuery("reference0"));
    ASSERT_TRUE(queried.ok());
    EXPECT_EQ("error", queried.value().get("/pipelines/reference0/state")
                           .value().as_string().value());
    EXPECT_EQ("reference",
              queried.value().get("/pipelines/reference0/error/provider")
                  .value().as_string().value());
    EXPECT_EQ("submit",
              queried.value().get("/pipelines/reference0/error/operation")
                  .value().as_string().value());
    EXPECT_EQ(eavp::HealthStatus::kError, platform.health().aggregate());
    EXPECT_EQ(2U, platform.metrics()
                      .counter("media.backend.instances.created").value());
    EXPECT_EQ(3U,
              platform.metrics().counter("media.frames.allocated").value());

    ASSERT_TRUE(platform.reset_pipeline().ok());
    queried = platform.query(eavp::PipelineStateQuery("reference0"));
    ASSERT_TRUE(queried.ok());
    EXPECT_EQ("running", queried.value().get("/pipelines/reference0/state")
                             .value().as_string().value());
    EXPECT_EQ(4U, platform.metrics()
                      .counter("media.backend.instances.created").value());
    ASSERT_TRUE(platform.tick(2U).ok());
    EXPECT_EQ(5U,
              platform.metrics().counter("media.frames.allocated").value());
    EXPECT_EQ(eavp::HealthStatus::kOk, platform.health().aggregate());
}

TEST(ReferenceMediaPlatformTest, ResetIsRejectedUnlessPipelineIsInError) {
    eavp::ReferenceMediaPlatform platform;
    ASSERT_TRUE(platform.initialize().ok());
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              platform.reset_pipeline().code());
}

TEST(ReferenceMediaPlatformTest,
     DrainBackpressureRemainsHealthyUntilRepeatedReconcileStopsPipeline) {
    eavp::ReferenceBackendOptions options;
    options.output_delay = 1U;
    eavp::ReferenceMediaPlatform platform(options);
    ASSERT_TRUE(platform.initialize().ok());
    ASSERT_TRUE(platform.dispatch(eavp::StartPipelineCommand(
        "start-delayed", "integration", "reference0")).ok());
    ASSERT_TRUE(platform.reconcile_once().ok());
    EXPECT_EQ(eavp::StatusCode::kWouldBlock, platform.tick(1U).code());
    EXPECT_EQ(eavp::HealthStatus::kOk, platform.health().aggregate());

    ASSERT_TRUE(platform.dispatch(eavp::StopPipelineCommand(
        "stop-delayed", "integration", "reference0")).ok());
    EXPECT_EQ(eavp::StatusCode::kWouldBlock,
              platform.reconcile_once().code());
    eavp::Result<eavp::StateSnapshot> queried =
        platform.query(eavp::PipelineStateQuery("reference0"));
    ASSERT_TRUE(queried.ok());
    EXPECT_EQ("draining", queried.value().get("/pipelines/reference0/state")
                              .value().as_string().value());
    EXPECT_EQ(eavp::HealthStatus::kOk, platform.health().aggregate());

    eavp::Status status(eavp::StatusCode::kWouldBlock);
    for (std::size_t turn = 0U;
         turn < 8U && status.code() == eavp::StatusCode::kWouldBlock;
         ++turn) {
        status = platform.reconcile_once();
    }
    ASSERT_TRUE(status.ok());
    queried = platform.query(eavp::PipelineStateQuery("reference0"));
    ASSERT_TRUE(queried.ok());
    EXPECT_EQ("stopped", queried.value().get("/pipelines/reference0/state")
                             .value().as_string().value());
    EXPECT_EQ(1U,
              platform.metrics().counter("media.frames.encoded").value());
    EXPECT_EQ(eavp::HealthStatus::kOk, platform.health().aggregate());
}

}  // namespace
