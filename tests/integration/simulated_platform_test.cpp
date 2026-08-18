#include <gtest/gtest.h>

#include "eavp/platform/simulated_platform.hpp"

namespace {

TEST(SimulatedPlatformTest, CommandToQueryFlowProcessesOneHundredPackets) {
    eavp::SimulatedPlatform platform;
    ASSERT_TRUE(platform.initialize().ok());

    ASSERT_TRUE(platform.dispatch(
        eavp::StartPipelineCommand("cmd-start", "integration-test", "live0")).ok());
    EXPECT_EQ(eavp::StatusCode::kNotFound,
              platform.query(eavp::PipelineStateQuery("live0")).status().code());

    ASSERT_TRUE(platform.reconcile_once().ok());
    ASSERT_TRUE(platform.tick(100U).ok());

    const eavp::Result<eavp::StateSnapshot> running =
        platform.query(eavp::PipelineStateQuery("live0"));
    ASSERT_TRUE(running.ok());
    EXPECT_EQ("running", running.value()
                             .get("/pipelines/live0/state")
                             .value()
                             .as_string()
                             .value());
    EXPECT_EQ(100U, platform.metrics().counter("media.packets.processed").value());
    EXPECT_DOUBLE_EQ(0.0, platform.metrics().gauge("pipeline.queue.depth").value());
    EXPECT_EQ(eavp::HealthStatus::kOk, platform.health().aggregate());

    ASSERT_TRUE(platform.dispatch(
        eavp::StopPipelineCommand("cmd-stop", "integration-test", "live0")).ok());
    ASSERT_TRUE(platform.reconcile_once().ok());
    EXPECT_EQ("stopped", platform.query(eavp::PipelineStateQuery("live0"))
                             .value()
                             .get("/pipelines/live0/state")
                             .value()
                             .as_string()
                             .value());
}

}  // namespace
