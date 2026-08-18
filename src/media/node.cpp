#include "eavp/media/node.hpp"

namespace eavp {

MediaNode::MediaNode(const std::string& id) : id_(id), state_(NodeState::kCreated) {}
MediaNode::~MediaNode() {}

const std::string& MediaNode::id() const { return id_; }
NodeState MediaNode::state() const { return state_; }

Status MediaNode::prepare() {
    if (state_ == NodeState::kPrepared) {
        return Status::ok_status();
    }
    if (state_ != NodeState::kCreated && state_ != NodeState::kStopped) {
        return Status(StatusCode::kInvalidState, "node cannot be prepared from current state");
    }
    const Status status = on_prepare();
    state_ = status.ok() ? NodeState::kPrepared : NodeState::kError;
    return status;
}

Status MediaNode::start() {
    if (state_ == NodeState::kRunning) {
        return Status::ok_status();
    }
    if (state_ != NodeState::kPrepared) {
        return Status(StatusCode::kInvalidState, "node is not prepared");
    }
    const Status status = on_start();
    state_ = status.ok() ? NodeState::kRunning : NodeState::kError;
    return status;
}

Status MediaNode::stop() {
    if (state_ == NodeState::kStopped) {
        return Status::ok_status();
    }
    if (state_ == NodeState::kCreated) {
        state_ = NodeState::kStopped;
        return Status::ok_status();
    }
    const Status status = on_stop();
    state_ = status.ok() ? NodeState::kStopped : NodeState::kError;
    return status;
}

Status MediaNode::reset() {
    const Status status = on_reset();
    state_ = status.ok() ? NodeState::kCreated : NodeState::kError;
    return status;
}

Status MediaNode::tick() {
    if (state_ != NodeState::kRunning) {
        return Status(StatusCode::kInvalidState, "node is not running");
    }
    const Status status = on_tick();
    if (!status.ok() && status.code() != StatusCode::kWouldBlock &&
        status.code() != StatusCode::kNotFound) {
        state_ = NodeState::kError;
    }
    return status;
}

Status MediaNode::on_prepare() { return Status::ok_status(); }
Status MediaNode::on_start() { return Status::ok_status(); }
Status MediaNode::on_stop() { return Status::ok_status(); }
Status MediaNode::on_reset() { return Status::ok_status(); }
Status MediaNode::on_tick() { return Status::ok_status(); }

}  // namespace eavp

