#include "eavp/media/pipeline.hpp"

#include <utility>

namespace eavp {

MediaPipeline::MediaPipeline(const std::string& id)
    : id_(id), state_(PipelineState::kCreated), drain_index_(0U) {}

MediaPipeline::~MediaPipeline() { stop(); }

const std::string& MediaPipeline::id() const { return id_; }
PipelineState MediaPipeline::state() const { return state_; }

Status MediaPipeline::add_node(std::unique_ptr<MediaNode> node) {
    if (!node) {
        return Status(StatusCode::kInvalidArgument, "node must not be null");
    }
    if (state_ == PipelineState::kRunning ||
        state_ == PipelineState::kDraining) {
        return Status(StatusCode::kInvalidState,
                      "cannot modify an active pipeline");
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
    if (state_ == PipelineState::kRunning ||
        state_ == PipelineState::kDraining) {
        return Status(StatusCode::kInvalidState,
                      "cannot modify an active pipeline");
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
        (*it)->reset();
    }
}

Status MediaPipeline::reset_nodes_reverse(const std::vector<MediaNode*>& nodes) {
    Status first_failure = Status::ok_status();
    for (std::vector<MediaNode*>::const_reverse_iterator it = nodes.rbegin();
         it != nodes.rend(); ++it) {
        const Status status = (*it)->reset();
        if (first_failure.ok() && !status.ok()) {
            first_failure = status;
        }
    }
    return first_failure;
}

Status MediaPipeline::tick_running_downstream(std::size_t current_index) {
    for (std::size_t index = current_index + 1U;
         index < drain_order_.size(); ++index) {
        if (drain_order_[index]->state() != NodeState::kRunning) {
            continue;
        }
        const Status status = drain_order_[index]->tick();
        if (!status.ok() && status.code() != StatusCode::kWouldBlock &&
            status.code() != StatusCode::kNotFound &&
            status.code() != StatusCode::kEndOfStream) {
            return status;
        }
    }
    return Status::ok_status();
}

Status MediaPipeline::start() {
    if (state_ == PipelineState::kRunning) {
        return Status::ok_status();
    }
    if (state_ == PipelineState::kDraining) {
        return Status(StatusCode::kInvalidState,
                      "pipeline is still draining");
    }
    if (state_ == PipelineState::kError) {
        return Status(StatusCode::kInvalidState, "pipeline must be reset after an error");
    }
    drain_order_.clear();
    drain_index_ = 0U;
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
    if (state_ != PipelineState::kDraining) {
        const Result<std::vector<std::string> > order_result =
            graph_.topological_order();
        if (!order_result.ok()) {
            state_ = PipelineState::kError;
            return order_result.status();
        }
        drain_order_ = ordered_nodes(order_result.value());
        drain_index_ = 0U;
        if (state_ == PipelineState::kError) {
            const Status reset_status = reset_nodes_reverse(drain_order_);
            state_ = reset_status.ok() ? PipelineState::kStopped
                                       : PipelineState::kError;
            return reset_status;
        }
        state_ = PipelineState::kDraining;
    }

    while (drain_index_ < drain_order_.size()) {
        const Status status = drain_order_[drain_index_]->stop();
        if (status.code() == StatusCode::kWouldBlock ||
            status.code() == StatusCode::kNotFound) {
            const Status downstream_status =
                tick_running_downstream(drain_index_);
            if (!downstream_status.ok()) {
                reset_nodes_reverse(drain_order_);
                state_ = PipelineState::kError;
                return downstream_status;
            }
            return Status(StatusCode::kWouldBlock,
                          "pipeline drain requires another executor turn");
        }
        if (!status.ok()) {
            reset_nodes_reverse(drain_order_);
            state_ = PipelineState::kError;
            return status;
        }
        ++drain_index_;
    }

    const Status reset_status = reset_nodes_reverse(drain_order_);
    if (!reset_status.ok()) {
        state_ = PipelineState::kError;
        return reset_status;
    }
    state_ = PipelineState::kStopped;
    drain_order_.clear();
    drain_index_ = 0U;
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
            status.code() != StatusCode::kNotFound &&
            status.code() != StatusCode::kEndOfStream) {
            state_ = PipelineState::kError;
            return status;
        }
    }
    return Status::ok_status();
}

}  // namespace eavp
