#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "eavp/media/graph.hpp"
#include "eavp/media/executor.hpp"
#include "eavp/media/media_packet.hpp"
#include "eavp/media/node.hpp"
#include "eavp/media/pipeline.hpp"
#include "eavp/media/port.hpp"
#include "eavp/media/queue.hpp"

namespace {

std::shared_ptr<const eavp::MediaPacket> make_packet(std::int64_t pts) {
    const eavp::Buffer buffer = eavp::Buffer::allocate(4).value();
    return std::shared_ptr<const eavp::MediaPacket>(new eavp::MediaPacket(
        buffer, eavp::CodecId::kH264, pts, pts, 1, eavp::TimeBase::create(1, 1000).value(),
        true));
}

TEST(QueueTest, BlockSignalsBackpressureWithoutDroppingQueuedPacket) {
    eavp::BoundedQueue<eavp::MediaPacket> queue(1, eavp::OverflowPolicy::kBlock);
    ASSERT_TRUE(queue.push(make_packet(1)).ok());

    const eavp::Status blocked = queue.push(make_packet(2));

    EXPECT_EQ(eavp::StatusCode::kWouldBlock, blocked.code());
    ASSERT_TRUE(queue.pop().ok());
    EXPECT_EQ(eavp::StatusCode::kNotFound, queue.pop().status().code());
}

TEST(QueueTest, DropOldestRetainsNewestPacket) {
    eavp::BoundedQueue<eavp::MediaPacket> queue(1, eavp::OverflowPolicy::kDropOldest);
    ASSERT_TRUE(queue.push(make_packet(10)).ok());
    ASSERT_TRUE(queue.push(make_packet(20)).ok());

    const eavp::Result<std::shared_ptr<const eavp::MediaPacket> > packet = queue.pop();

    ASSERT_TRUE(packet.ok());
    EXPECT_EQ(20, packet.value()->pts());
    EXPECT_EQ(1U, queue.dropped_count());
}

TEST(QueueTest, DropNewestKeepsAlreadyQueuedPacket) {
    eavp::BoundedQueue<eavp::MediaPacket> queue(1, eavp::OverflowPolicy::kDropNewest);
    ASSERT_TRUE(queue.push(make_packet(10)).ok());
    ASSERT_TRUE(queue.push(make_packet(20)).ok());

    const eavp::Result<std::shared_ptr<const eavp::MediaPacket> > packet = queue.pop();
    ASSERT_TRUE(packet.ok());
    EXPECT_EQ(10, packet.value()->pts());
    EXPECT_EQ(1U, queue.dropped_count());
}

TEST(PortTest, ConnectedPortsTransferTheSameImmutablePacket) {
    eavp::OutputPort<eavp::MediaPacket> output("encoded");
    eavp::InputPort<eavp::MediaPacket> input("mux", 2, eavp::OverflowPolicy::kBlock);
    ASSERT_TRUE(eavp::connect(output, input).ok());
    const std::shared_ptr<const eavp::MediaPacket> packet = make_packet(30);

    ASSERT_TRUE(output.send(packet).ok());
    const eavp::Result<std::shared_ptr<const eavp::MediaPacket> > received = input.receive();

    ASSERT_TRUE(received.ok());
    EXPECT_EQ(packet.get(), received.value().get());
}

TEST(GraphTest, RejectsAnEdgeThatWouldCreateACycle) {
    eavp::MediaGraph graph;
    ASSERT_TRUE(graph.add_node("source").ok());
    ASSERT_TRUE(graph.add_node("filter").ok());
    ASSERT_TRUE(graph.add_node("sink").ok());
    ASSERT_TRUE(graph.connect("source", "filter").ok());
    ASSERT_TRUE(graph.connect("filter", "sink").ok());

    EXPECT_EQ(eavp::StatusCode::kInvalidArgument, graph.connect("sink", "source").code());
}

class RecordingNode : public eavp::MediaNode {
public:
    RecordingNode(const std::string& id, std::vector<std::string>* events, bool fail_start)
        : eavp::MediaNode(id), events_(events), fail_start_(fail_start), starts_(0), stops_(0) {}

    int starts() const { return starts_; }
    int stops() const { return stops_; }

protected:
    eavp::Status on_prepare() override {
        events_->push_back(id() + ":prepare");
        return eavp::Status::ok_status();
    }

    eavp::Status on_start() override {
        ++starts_;
        events_->push_back(id() + (fail_start_ ? ":start_fail" : ":start"));
        return fail_start_ ? eavp::Status(eavp::StatusCode::kInternal, "start failed")
                           : eavp::Status::ok_status();
    }

    eavp::Status on_stop() override {
        ++stops_;
        events_->push_back(id() + ":stop");
        return eavp::Status::ok_status();
    }

private:
    std::vector<std::string>* events_;
    bool fail_start_;
    int starts_;
    int stops_;
};

TEST(PipelineTest, RollsBackPreparedNodesInReverseOrderWhenStartFails) {
    std::vector<std::string> events;
    eavp::MediaPipeline pipeline("live0");
    ASSERT_TRUE(pipeline.add_node(
        std::unique_ptr<eavp::MediaNode>(new RecordingNode("source", &events, false))).ok());
    ASSERT_TRUE(pipeline.add_node(
        std::unique_ptr<eavp::MediaNode>(new RecordingNode("sink", &events, true))).ok());
    ASSERT_TRUE(pipeline.connect("source", "sink").ok());

    EXPECT_EQ(eavp::StatusCode::kInternal, pipeline.start().code());

    const std::vector<std::string> expected = {"source:prepare", "sink:prepare", "source:start",
                                               "sink:start_fail", "sink:stop", "source:stop"};
    EXPECT_EQ(expected, events);
    EXPECT_EQ(eavp::PipelineState::kError, pipeline.state());
}

TEST(PipelineTest, RepeatedStartAndStopDoNotRepeatNodeSideEffects) {
    std::vector<std::string> events;
    eavp::MediaPipeline pipeline("live0");
    RecordingNode* node = new RecordingNode("source", &events, false);
    ASSERT_TRUE(pipeline.add_node(std::unique_ptr<eavp::MediaNode>(node)).ok());

    ASSERT_TRUE(pipeline.start().ok());
    ASSERT_TRUE(pipeline.start().ok());
    ASSERT_TRUE(pipeline.stop().ok());
    ASSERT_TRUE(pipeline.stop().ok());

    EXPECT_EQ(1, node->starts());
    EXPECT_EQ(1, node->stops());
}

class FailingTickNode : public eavp::MediaNode {
public:
    FailingTickNode() : eavp::MediaNode("tick"), ticks_(0) {}
    int ticks() const { return ticks_; }

protected:
    eavp::Status on_tick() override {
        ++ticks_;
        return ticks_ == 3 ? eavp::Status(eavp::StatusCode::kInternal, "tick failed")
                           : eavp::Status::ok_status();
    }

private:
    int ticks_;
};

TEST(ExecutorTest, StopsAtFirstPipelineTickFailure) {
    eavp::MediaPipeline pipeline("live0");
    FailingTickNode* node = new FailingTickNode();
    ASSERT_TRUE(pipeline.add_node(std::unique_ptr<eavp::MediaNode>(node)).ok());
    ASSERT_TRUE(pipeline.start().ok());
    eavp::DeterministicExecutor executor;

    EXPECT_EQ(eavp::StatusCode::kInternal, executor.run(&pipeline, 10U).code());
    EXPECT_EQ(3, node->ticks());
    EXPECT_EQ(eavp::PipelineState::kError, pipeline.state());
}

}  // namespace
