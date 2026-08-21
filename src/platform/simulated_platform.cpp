#include "eavp/platform/simulated_platform.hpp"

#include <cstdint>
#include <memory>

#include "eavp/media/media_packet.hpp"
#include "eavp/media/port.hpp"

namespace eavp {

namespace {

class FakeSourceNode : public MediaNode {
public:
    FakeSourceNode()
        : MediaNode("source"), output_("packet_output"), next_pts_(0),
          time_base_(TimeBase::create(1, 1000).value()) {}

    OutputPort<MediaPacket>& output() { return output_; }

protected:
    Status on_tick() override {
        Result<Buffer> buffer_result = Buffer::allocate(16U);
        if (!buffer_result.ok()) {
            return buffer_result.status();
        }
        Buffer buffer = buffer_result.take_value();
        Result<MappedRegion> mapped_result = buffer.map_plane(0U, MapMode::kReadWrite);
        if (!mapped_result.ok()) {
            return mapped_result.status();
        }
        MappedRegion mapped = mapped_result.take_value();
        mapped.mutable_data()[0] = static_cast<std::uint8_t>(next_pts_ & 0xff);
        Result<MediaPacket> packet_result = MediaPacket::create(
            buffer, CodecId::kReference, EncodedStreamFormat::kReference, 0, next_pts_,
            next_pts_, 1, time_base_, true, CodecConfigData());
        if (!packet_result.ok()) {
            return packet_result.status();
        }
        const std::shared_ptr<const MediaPacket> packet(
            new MediaPacket(packet_result.take_value()));
        ++next_pts_;
        return output_.send(packet);
    }

private:
    OutputPort<MediaPacket> output_;
    std::int64_t next_pts_;
    TimeBase time_base_;
};

class PassThroughNode : public MediaNode {
public:
    PassThroughNode()
        : MediaNode("passthrough"),
          input_("packet_input", 4U, OverflowPolicy::kBlock),
          output_("packet_output") {}

    InputPort<MediaPacket>& input() { return input_; }
    OutputPort<MediaPacket>& output() { return output_; }

protected:
    Status on_tick() override {
        const Result<std::shared_ptr<const MediaPacket> > packet = input_.receive();
        if (!packet.ok()) {
            return packet.status().code() == StatusCode::kNotFound ? Status::ok_status()
                                                                    : packet.status();
        }
        return output_.send(packet.value());
    }

private:
    InputPort<MediaPacket> input_;
    OutputPort<MediaPacket> output_;
};

class FakeSinkNode : public MediaNode {
public:
    explicit FakeSinkNode(MetricRegistry* metrics)
        : MediaNode("sink"), input_("packet_input", 4U, OverflowPolicy::kBlock),
          metrics_(metrics) {}

    InputPort<MediaPacket>& input() { return input_; }

protected:
    Status on_tick() override {
        const Result<std::shared_ptr<const MediaPacket> > packet = input_.receive();
        if (!packet.ok()) {
            return packet.status().code() == StatusCode::kNotFound ? Status::ok_status()
                                                                    : packet.status();
        }
        const Status counter_status = metrics_->increment_counter("media.packets.processed");
        if (!counter_status.ok()) {
            return counter_status;
        }
        return metrics_->set_gauge("pipeline.queue.depth",
                                   static_cast<double>(input_.queue_size()));
    }

private:
    InputPort<MediaPacket> input_;
    MetricRegistry* metrics_;
};

}  // namespace

SimulatedPlatform::SimulatedPlatform()
    : command_handler_(&desired_),
      pipeline_("live0"),
      reconciler_(&pipeline_, &desired_, &actual_),
      initialized_(false) {}

Status SimulatedPlatform::initialize() {
    if (initialized_) {
        return Status::ok_status();
    }
    std::unique_ptr<FakeSourceNode> source(new FakeSourceNode());
    std::unique_ptr<PassThroughNode> passthrough(new PassThroughNode());
    std::unique_ptr<FakeSinkNode> sink(new FakeSinkNode(&metrics_));

    Status status = connect(source->output(), passthrough->input());
    if (!status.ok()) {
        return status;
    }
    status = connect(passthrough->output(), sink->input());
    if (!status.ok()) {
        return status;
    }
    status = pipeline_.add_node(std::unique_ptr<MediaNode>(source.release()));
    if (!status.ok()) {
        return status;
    }
    status = pipeline_.add_node(std::unique_ptr<MediaNode>(passthrough.release()));
    if (!status.ok()) {
        return status;
    }
    status = pipeline_.add_node(std::unique_ptr<MediaNode>(sink.release()));
    if (!status.ok()) {
        return status;
    }
    status = pipeline_.connect("source", "passthrough");
    if (!status.ok()) {
        return status;
    }
    status = pipeline_.connect("passthrough", "sink");
    if (!status.ok()) {
        return status;
    }
    initialized_ = true;
    return health_.report("pipeline", HealthStatus::kOk, "initialized");
}

Status SimulatedPlatform::dispatch(const StartPipelineCommand& command) {
    return initialized_ ? command_handler_.handle(command)
                        : Status(StatusCode::kInvalidState, "platform is not initialized");
}

Status SimulatedPlatform::dispatch(const StopPipelineCommand& command) {
    return initialized_ ? command_handler_.handle(command)
                        : Status(StatusCode::kInvalidState, "platform is not initialized");
}

Status SimulatedPlatform::reconcile_once() {
    if (!initialized_) {
        return Status(StatusCode::kInvalidState, "platform is not initialized");
    }
    const Status status = reconciler_.reconcile_once();
    if (!status.ok()) {
        const Status health_status =
            health_.report("pipeline", HealthStatus::kError, status.message());
        return health_status.ok() ? status : health_status;
    }
    return health_.report("pipeline", HealthStatus::kOk,
                          "desired state converged");
}

Status SimulatedPlatform::tick(std::size_t count) {
    if (!initialized_) {
        return Status(StatusCode::kInvalidState, "platform is not initialized");
    }
    const Status status = executor_.run(&pipeline_, count);
    if (!status.ok()) {
        const Status health_status =
            health_.report("pipeline", HealthStatus::kError, status.message());
        return health_status.ok() ? status : health_status;
    }
    return health_.report("pipeline", HealthStatus::kOk, "running");
}

Result<StateSnapshot> SimulatedPlatform::query(const PipelineStateQuery& query) const {
    if (query.pipeline_id != pipeline_.id()) {
        return Result<StateSnapshot>(Status(StatusCode::kNotFound, "pipeline was not found"));
    }
    const StateSnapshot snapshot = actual_.snapshot();
    const Result<StateValue> state = snapshot.get("/pipelines/" + query.pipeline_id + "/state");
    if (!state.ok()) {
        return Result<StateSnapshot>(state.status());
    }
    return Result<StateSnapshot>(snapshot);
}

const MetricRegistry& SimulatedPlatform::metrics() const { return metrics_; }
const HealthManager& SimulatedPlatform::health() const { return health_; }

}  // namespace eavp
