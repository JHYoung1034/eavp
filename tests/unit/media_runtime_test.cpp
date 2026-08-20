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
    eavp::Result<eavp::Buffer> allocated = eavp::Buffer::allocate(4U);
    if (!allocated.ok()) {
        ADD_FAILURE() << allocated.status().message();
        return std::shared_ptr<const eavp::MediaPacket>();
    }
    const eavp::Buffer buffer = allocated.take_value();
    eavp::Result<eavp::MediaPacket> packet = eavp::MediaPacket::create(
        buffer, eavp::CodecId::kReference, eavp::EncodedStreamFormat::kReference, 0, pts, pts,
        1, eavp::TimeBase::create(1, 1000).take_value(), true, eavp::CodecConfigData());
    if (!packet.ok()) {
        ADD_FAILURE() << packet.status().message();
        return std::shared_ptr<const eavp::MediaPacket>();
    }
    return std::shared_ptr<const eavp::MediaPacket>(new eavp::MediaPacket(packet.take_value()));
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

TEST(PortTest, FanOutDoesNotPartiallyPushWhenALaterBlockQueueIsFull) {
    eavp::OutputPort<eavp::MediaPacket> output("encoded");
    eavp::InputPort<eavp::MediaPacket> first(
        "first", 2U, eavp::OverflowPolicy::kBlock);
    eavp::InputPort<eavp::MediaPacket> second(
        "second", 1U, eavp::OverflowPolicy::kBlock);
    ASSERT_TRUE(eavp::connect(output, first).ok());
    ASSERT_TRUE(eavp::connect(output, second).ok());
    ASSERT_TRUE(output.send(make_packet(0)).ok());
    ASSERT_TRUE(first.receive().ok());

    EXPECT_EQ(eavp::StatusCode::kWouldBlock,
              output.send(make_packet(1)).code());
    EXPECT_EQ(0U, first.queue_size());
    EXPECT_EQ(1U, second.queue_size());
    ASSERT_TRUE(second.receive().ok());

    ASSERT_TRUE(output.send(make_packet(1)).ok());
    eavp::Result<std::shared_ptr<const eavp::MediaPacket> > first_packet =
        first.receive();
    eavp::Result<std::shared_ptr<const eavp::MediaPacket> > second_packet =
        second.receive();
    ASSERT_TRUE(first_packet.ok());
    ASSERT_TRUE(second_packet.ok());
    EXPECT_EQ(1, first_packet.value()->pts());
    EXPECT_EQ(1, second_packet.value()->pts());
    EXPECT_EQ(eavp::StatusCode::kNotFound, first.receive().status().code());
    EXPECT_EQ(eavp::StatusCode::kNotFound, second.receive().status().code());
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

class AsynchronousDrainNode : public eavp::MediaNode {
public:
    AsynchronousDrainNode(const std::string& id, int blocked_stops,
                          std::vector<std::string>* events)
        : eavp::MediaNode(id), blocked_stops_(blocked_stops),
          stop_calls_(0), reset_calls_(0), events_(events) {}

    int reset_calls() const { return reset_calls_; }

protected:
    eavp::Status on_stop() override {
        events_->push_back(id() + ":drain");
        if (stop_calls_++ < blocked_stops_) {
            return eavp::Status(eavp::StatusCode::kWouldBlock,
                                "drain needs another executor turn");
        }
        return eavp::Status(eavp::StatusCode::kEndOfStream,
                            "drain reached end of stream");
    }

    eavp::Status on_reset() override {
        ++reset_calls_;
        events_->push_back(id() + ":reset");
        return eavp::Status::ok_status();
    }

private:
    int blocked_stops_;
    int stop_calls_;
    int reset_calls_;
    std::vector<std::string>* events_;
};

class BackpressuredDrainSource : public eavp::MediaNode {
public:
    BackpressuredDrainSource()
        : eavp::MediaNode("source"), output_("packet_output") {
        backlog_.push_back(make_packet(1));
        backlog_.push_back(make_packet(2));
    }

    eavp::OutputPort<eavp::MediaPacket>& output() { return output_; }

protected:
    eavp::Status on_stop() override {
        if (backlog_.empty()) {
            return eavp::Status(eavp::StatusCode::kEndOfStream);
        }
        const eavp::Status status = output_.send(backlog_.front());
        if (status.ok()) {
            backlog_.erase(backlog_.begin());
            return eavp::Status(eavp::StatusCode::kWouldBlock);
        }
        return status;
    }

private:
    eavp::OutputPort<eavp::MediaPacket> output_;
    std::vector<std::shared_ptr<const eavp::MediaPacket> > backlog_;
};

class BackpressuredDrainForward : public eavp::MediaNode {
public:
    BackpressuredDrainForward()
        : eavp::MediaNode("forward"),
          input_("packet_input", 1U, eavp::OverflowPolicy::kBlock),
          output_("packet_output") {}

    eavp::InputPort<eavp::MediaPacket>& input() { return input_; }
    eavp::OutputPort<eavp::MediaPacket>& output() { return output_; }

protected:
    eavp::Status transfer_one() {
        if (pending_) {
            const eavp::Status status = output_.send(pending_);
            if (!status.ok()) {
                return status;
            }
            pending_.reset();
        }
        eavp::Result<std::shared_ptr<const eavp::MediaPacket> > packet =
            input_.receive();
        if (!packet.ok()) {
            return packet.status().code() == eavp::StatusCode::kNotFound
                       ? eavp::Status::ok_status()
                       : packet.status();
        }
        pending_ = packet.take_value();
        const eavp::Status status = output_.send(pending_);
        if (status.ok()) {
            pending_.reset();
        }
        return status;
    }

    eavp::Status on_tick() override { return transfer_one(); }

    eavp::Status on_stop() override {
        const bool had_work = pending_ || input_.queue_size() != 0U;
        const eavp::Status status = transfer_one();
        if (!status.ok()) {
            return status;
        }
        return had_work ? eavp::Status(eavp::StatusCode::kWouldBlock)
                        : eavp::Status(eavp::StatusCode::kEndOfStream);
    }

private:
    eavp::InputPort<eavp::MediaPacket> input_;
    eavp::OutputPort<eavp::MediaPacket> output_;
    std::shared_ptr<const eavp::MediaPacket> pending_;
};

class BackpressuredDrainSink : public eavp::MediaNode {
public:
    BackpressuredDrainSink()
        : eavp::MediaNode("sink"),
          input_("packet_input", 1U, eavp::OverflowPolicy::kBlock) {}

    eavp::InputPort<eavp::MediaPacket>& input() { return input_; }
    const std::vector<std::int64_t>& received_pts() const {
        return received_pts_;
    }

protected:
    eavp::Status consume_one() {
        eavp::Result<std::shared_ptr<const eavp::MediaPacket> > packet =
            input_.receive();
        if (!packet.ok()) {
            return packet.status().code() == eavp::StatusCode::kNotFound
                       ? eavp::Status::ok_status()
                       : packet.status();
        }
        received_pts_.push_back(packet.value()->pts());
        return eavp::Status::ok_status();
    }

    eavp::Status on_tick() override { return consume_one(); }

    eavp::Status on_stop() override {
        if (input_.queue_size() == 0U) {
            return eavp::Status(eavp::StatusCode::kEndOfStream);
        }
        const eavp::Status status = consume_one();
        return status.ok() ? eavp::Status(eavp::StatusCode::kWouldBlock)
                           : status;
    }

private:
    eavp::InputPort<eavp::MediaPacket> input_;
    std::vector<std::int64_t> received_pts_;
};

class AsyncRollbackNode : public eavp::MediaNode {
public:
    AsyncRollbackNode(const std::string& id, bool fail_start,
                      std::vector<std::string>* events)
        : eavp::MediaNode(id), fail_start_(fail_start), events_(events),
          resets_(0) {}

    int resets() const { return resets_; }

protected:
    eavp::Status on_prepare() override {
        events_->push_back(id() + ":prepare");
        return eavp::Status::ok_status();
    }

    eavp::Status on_start() override {
        events_->push_back(id() + (fail_start_ ? ":start_fail" : ":start"));
        return fail_start_
                   ? eavp::Status(eavp::StatusCode::kInternal,
                                  "original start failure")
                   : eavp::Status::ok_status();
    }

    eavp::Status on_stop() override {
        events_->push_back(id() + ":stop_blocked");
        return eavp::Status(eavp::StatusCode::kWouldBlock);
    }

    eavp::Status on_reset() override {
        ++resets_;
        events_->push_back(id() + ":reset");
        return eavp::Status::ok_status();
    }

private:
    bool fail_start_;
    std::vector<std::string>* events_;
    int resets_;
};

TEST(PipelineTest, DrainTicksRunningDownstreamThroughTwoBoundedQueues) {
    eavp::MediaPipeline pipeline("drain-ports");
    BackpressuredDrainSource* source = new BackpressuredDrainSource();
    BackpressuredDrainForward* forward = new BackpressuredDrainForward();
    BackpressuredDrainSink* sink = new BackpressuredDrainSink();
    ASSERT_TRUE(eavp::connect(source->output(), forward->input()).ok());
    ASSERT_TRUE(eavp::connect(forward->output(), sink->input()).ok());
    ASSERT_TRUE(source->output().send(make_packet(0)).ok());
    ASSERT_TRUE(forward->output().send(make_packet(-1)).ok());
    ASSERT_TRUE(pipeline.add_node(
        std::unique_ptr<eavp::MediaNode>(source)).ok());
    ASSERT_TRUE(pipeline.add_node(
        std::unique_ptr<eavp::MediaNode>(forward)).ok());
    ASSERT_TRUE(pipeline.add_node(
        std::unique_ptr<eavp::MediaNode>(sink)).ok());
    ASSERT_TRUE(pipeline.connect("source", "forward").ok());
    ASSERT_TRUE(pipeline.connect("forward", "sink").ok());
    ASSERT_TRUE(pipeline.start().ok());

    eavp::Status status(eavp::StatusCode::kWouldBlock);
    for (std::size_t turn = 0U;
         turn < 16U && status.code() == eavp::StatusCode::kWouldBlock;
         ++turn) {
        status = pipeline.stop();
    }

    ASSERT_TRUE(status.ok());
    EXPECT_EQ(eavp::PipelineState::kStopped, pipeline.state());
    const std::vector<std::int64_t> expected{-1, 0, 1, 2};
    EXPECT_EQ(expected, sink->received_pts());
}

TEST(PipelineTest, StartFailureResetsNodesWhoseRollbackStopWouldBlock) {
    std::vector<std::string> events;
    eavp::MediaPipeline pipeline("rollback");
    AsyncRollbackNode* source =
        new AsyncRollbackNode("source", false, &events);
    AsyncRollbackNode* sink =
        new AsyncRollbackNode("sink", true, &events);
    ASSERT_TRUE(pipeline.add_node(
        std::unique_ptr<eavp::MediaNode>(source)).ok());
    ASSERT_TRUE(pipeline.add_node(
        std::unique_ptr<eavp::MediaNode>(sink)).ok());
    ASSERT_TRUE(pipeline.connect("source", "sink").ok());

    const eavp::Status status = pipeline.start();

    EXPECT_EQ(eavp::StatusCode::kInternal, status.code());
    EXPECT_EQ("original start failure", status.message());
    const std::vector<std::string> expected{
        "source:prepare", "sink:prepare", "source:start", "sink:start_fail",
        "sink:stop_blocked", "sink:reset", "source:stop_blocked",
        "source:reset"};
    EXPECT_EQ(expected, events);
    EXPECT_EQ(1, source->resets());
    EXPECT_EQ(1, sink->resets());
    EXPECT_EQ(eavp::PipelineState::kError, pipeline.state());
}

TEST(PipelineTest, StopsSubmissionThenContinuesTopologicalDrainAcrossCalls) {
    std::vector<std::string> events;
    eavp::MediaPipeline pipeline("drain");
    AsynchronousDrainNode* source =
        new AsynchronousDrainNode("source", 0, &events);
    AsynchronousDrainNode* processor =
        new AsynchronousDrainNode("processor", 1, &events);
    AsynchronousDrainNode* sink =
        new AsynchronousDrainNode("sink", 1, &events);
    ASSERT_TRUE(pipeline.add_node(
        std::unique_ptr<eavp::MediaNode>(source)).ok());
    ASSERT_TRUE(pipeline.add_node(
        std::unique_ptr<eavp::MediaNode>(processor)).ok());
    ASSERT_TRUE(pipeline.add_node(
        std::unique_ptr<eavp::MediaNode>(sink)).ok());
    ASSERT_TRUE(pipeline.connect("source", "processor").ok());
    ASSERT_TRUE(pipeline.connect("processor", "sink").ok());
    ASSERT_TRUE(pipeline.start().ok());

    EXPECT_EQ(eavp::StatusCode::kWouldBlock, pipeline.stop().code());
    EXPECT_EQ(eavp::PipelineState::kDraining, pipeline.state());
    const std::vector<std::string> first_turn{
        "source:drain", "processor:drain"};
    EXPECT_EQ(first_turn, events);

    EXPECT_EQ(eavp::StatusCode::kWouldBlock, pipeline.stop().code());
    EXPECT_EQ(eavp::PipelineState::kDraining, pipeline.state());
    ASSERT_TRUE(pipeline.stop().ok());
    EXPECT_EQ(eavp::PipelineState::kStopped, pipeline.state());

    const std::vector<std::string> expected{
        "source:drain", "processor:drain", "processor:drain",
        "sink:drain", "sink:drain", "sink:reset", "processor:reset",
        "source:reset"};
    EXPECT_EQ(expected, events);
    EXPECT_EQ(1, source->reset_calls());
    EXPECT_EQ(1, processor->reset_calls());
    EXPECT_EQ(1, sink->reset_calls());
    ASSERT_TRUE(pipeline.stop().ok());
    EXPECT_EQ(expected, events);
}

TEST(PipelineTest, RejectsStartAndGraphMutationWhileDraining) {
    std::vector<std::string> events;
    eavp::MediaPipeline pipeline("drain");
    ASSERT_TRUE(pipeline.add_node(std::unique_ptr<eavp::MediaNode>(
        new AsynchronousDrainNode("source", 1, &events))).ok());
    ASSERT_TRUE(pipeline.start().ok());
    EXPECT_EQ(eavp::StatusCode::kWouldBlock, pipeline.stop().code());
    ASSERT_EQ(eavp::PipelineState::kDraining, pipeline.state());

    EXPECT_EQ(eavp::StatusCode::kInvalidState, pipeline.start().code());
    EXPECT_EQ(eavp::PipelineState::kDraining, pipeline.state());
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              pipeline.add_node(std::unique_ptr<eavp::MediaNode>(
                  new RecordingNode("late", &events, false))).code());
    EXPECT_EQ(eavp::PipelineState::kDraining, pipeline.state());
}

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
