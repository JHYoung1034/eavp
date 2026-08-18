#include "eavp/media/pipeline.hpp"

#include <utility>

namespace eavp {

MediaPipeline::MediaPipeline(const std::string& id) : id_(id), state_(PipelineState::kCreated) {}

MediaPipeline::~MediaPipeline() { stop(); }

const std::string& MediaPipeline::id() const { return id_; }
PipelineState MediaPipeline::state() const { return state_; }

Status MediaPipeline::add_node(std::unique_ptr<MediaNode> node) {
    if (!node) {
        return Status(StatusCode::kInvalidArgument, "node must not be null");
    }
    if (state_ == PipelineState::kRunning) {
        return Status(StatusCode::kInvalidState, "cannot modify a running pipeline");
    }
    const std::string node_id = node->id();
    const Status graph_status = graph_.add_node(node_id);
    if (!graph_status.ok()) {
        return graph_status;
    }
    nodes_by_id_[node_id] = node.get();
    nodes_.push_back(std::move(node));
    return Status::ok_status();
}

Status MediaPipeline::connect(const std::string& source, const std::string& sink) {
    if (state_ == PipelineState::kRunning) {
        return Status(StatusCode::kInvalidState, "cannot modify a running pipeline");
    }
    return graph_.connect(source, sink);
}

std::vector<MediaNode*> MediaPipeline::ordered_nodes(
    const std::vector<std::string>& order) const {
    std::vector<MediaNode*> result;
    for (std::vector<std::string>::const_iterator it = order.begin(); it != order.end(); ++it) {
        result.push_back(nodes_by_id_.find(*it)->second);
    }
    return result;
}

void MediaPipeline::stop_nodes_reverse(const std::vector<MediaNode*>& nodes) {
    for (std::vector<MediaNode*>::const_reverse_iterator it = nodes.rbegin(); it != nodes.rend();
         ++it) {
        (*it)->stop();
    }
}

Status MediaPipeline::start() {
    if (state_ == PipelineState::kRunning) {
        return Status::ok_status();
    }
    if (state_ == PipelineState::kError) {
        return Status(StatusCode::kInvalidState, "pipeline must be reset after an error");
    }
    const Result<std::vector<std::string> > order_result = graph_.topological_order();
    if (!order_result.ok()) {
        state_ = PipelineState::kError;
        return order_result.status();
    }
    if (order_result.value().empty()) {
        state_ = PipelineState::kError;
        return Status(StatusCode::kInvalidState, "pipeline has no nodes");
    }
    const std::vector<MediaNode*> order = ordered_nodes(order_result.value());

    std::vector<MediaNode*> prepared;
    for (std::vector<MediaNode*>::const_iterator it = order.begin(); it != order.end(); ++it) {
        const Status status = (*it)->prepare();
        if (!status.ok()) {
            stop_nodes_reverse(prepared);
            state_ = PipelineState::kError;
            return status;
        }
        prepared.push_back(*it);
    }

    for (std::vector<MediaNode*>::const_iterator it = order.begin(); it != order.end(); ++it) {
        const Status status = (*it)->start();
        if (!status.ok()) {
            stop_nodes_reverse(prepared);
            state_ = PipelineState::kError;
            return status;
        }
    }
    state_ = PipelineState::kRunning;
    return Status::ok_status();
}

Status MediaPipeline::stop() {
    if (state_ == PipelineState::kStopped) {
        return Status::ok_status();
    }
    if (state_ == PipelineState::kCreated) {
        state_ = PipelineState::kStopped;
        return Status::ok_status();
    }
    const Result<std::vector<std::string> > order_result = graph_.topological_order();
    if (!order_result.ok()) {
        state_ = PipelineState::kError;
        return order_result.status();
    }
    const std::vector<MediaNode*> order = ordered_nodes(order_result.value());
    for (std::vector<MediaNode*>::const_reverse_iterator it = order.rbegin(); it != order.rend();
         ++it) {
        const Status status = (*it)->stop();
        if (!status.ok()) {
            state_ = PipelineState::kError;
            return status;
        }
    }
    state_ = PipelineState::kStopped;
    return Status::ok_status();
}

Status MediaPipeline::tick() {
    if (state_ != PipelineState::kRunning) {
        return Status(StatusCode::kInvalidState, "pipeline is not running");
    }
    const Result<std::vector<std::string> > order_result = graph_.topological_order();
    if (!order_result.ok()) {
        return order_result.status();
    }
    const std::vector<MediaNode*> order = ordered_nodes(order_result.value());
    for (std::vector<MediaNode*>::const_iterator it = order.begin(); it != order.end(); ++it) {
        const Status status = (*it)->tick();
        if (!status.ok() && status.code() != StatusCode::kWouldBlock &&
            status.code() != StatusCode::kNotFound) {
            state_ = PipelineState::kError;
            return status;
        }
    }
    return Status::ok_status();
}

}  // namespace eavp

