#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "eavp/control/command.hpp"
#include "eavp/control/pipeline_reconciler.hpp"
#include "eavp/control/state_store.hpp"

namespace {

TEST(StateStoreTest, DesiredAndActualStoresHaveIndependentVersionedSnapshots) {
    eavp::StateStore desired;
    eavp::StateStore actual;
    ASSERT_TRUE(desired.set("/pipelines/live0/state", eavp::StateValue("running")).ok());
    const eavp::StateSnapshot first = desired.snapshot();
    ASSERT_TRUE(desired.set("/pipelines/live0/state", eavp::StateValue("running")).ok());

    EXPECT_EQ(1U, desired.version());
    EXPECT_EQ(0U, actual.version());
    ASSERT_TRUE(first.get("/pipelines/live0/state").ok());
    EXPECT_EQ("running", first.get("/pipelines/live0/state").value().as_string().value());
    EXPECT_EQ(eavp::StatusCode::kNotFound,
              actual.snapshot().get("/pipelines/live0/state").status().code());

    ASSERT_TRUE(desired.set("/pipelines/live0/state", eavp::StateValue("stopped")).ok());
    EXPECT_EQ("running", first.get("/pipelines/live0/state").value().as_string().value());
    EXPECT_EQ(2U, desired.version());
}

class CountingNode : public eavp::MediaNode {
public:
    CountingNode(const std::string& id, bool fail_start)
        : eavp::MediaNode(id), fail_start_(fail_start), starts_(0) {}

    int starts() const { return starts_; }

protected:
    eavp::Status on_start() override {
        ++starts_;
        return fail_start_ ? eavp::Status(eavp::StatusCode::kInternal, "device rejected start")
                           : eavp::Status::ok_status();
    }

private:
    bool fail_start_;
    int starts_;
};

TEST(ControlTest, CommandChangesDesiredAndReconcilerConvergesOnlyOnce) {
    eavp::StateStore desired;
    eavp::StateStore actual;
    eavp::PipelineCommandHandler commands(&desired);
    eavp::MediaPipeline pipeline("live0");
    CountingNode* node = new CountingNode("source", false);
    ASSERT_TRUE(pipeline.add_node(std::unique_ptr<eavp::MediaNode>(node)).ok());
    eavp::PipelineReconciler reconciler(&pipeline, &desired, &actual);

    ASSERT_TRUE(commands.handle(eavp::StartPipelineCommand("cmd-1", "test", "live0")).ok());
    EXPECT_EQ(eavp::PipelineState::kCreated, pipeline.state());
    ASSERT_TRUE(reconciler.reconcile_once().ok());
    ASSERT_TRUE(reconciler.reconcile_once().ok());

    EXPECT_EQ(eavp::PipelineState::kRunning, pipeline.state());
    EXPECT_EQ(1, node->starts());
    EXPECT_EQ("running", actual.snapshot()
                             .get("/pipelines/live0/state")
                             .value()
                             .as_string()
                             .value());

    ASSERT_TRUE(commands.handle(eavp::StopPipelineCommand("cmd-2", "test", "live0")).ok());
    ASSERT_TRUE(reconciler.reconcile_once().ok());
    EXPECT_EQ(eavp::PipelineState::kStopped, pipeline.state());
}

TEST(ControlTest, FailedReconcileKeepsDesiredAndPublishesActualError) {
    eavp::StateStore desired;
    eavp::StateStore actual;
    eavp::PipelineCommandHandler commands(&desired);
    eavp::MediaPipeline pipeline("live0");
    ASSERT_TRUE(pipeline.add_node(
        std::unique_ptr<eavp::MediaNode>(new CountingNode("source", true))).ok());
    eavp::PipelineReconciler reconciler(&pipeline, &desired, &actual);
    ASSERT_TRUE(commands.handle(eavp::StartPipelineCommand("cmd-1", "test", "live0")).ok());

    EXPECT_EQ(eavp::StatusCode::kInternal, reconciler.reconcile_once().code());

    EXPECT_EQ("running", desired.snapshot()
                             .get("/pipelines/live0/state")
                             .value()
                             .as_string()
                             .value());
    EXPECT_EQ("error", actual.snapshot()
                           .get("/pipelines/live0/state")
                           .value()
                           .as_string()
                           .value());
    EXPECT_EQ("device rejected start", actual.snapshot()
                                           .get("/pipelines/live0/error")
                                           .value()
                                           .as_string()
                                           .value());
}

}  // namespace
