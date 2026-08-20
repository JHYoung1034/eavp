#ifndef EAVP_MEDIA_PORT_HPP_
#define EAVP_MEDIA_PORT_HPP_

#include <memory>
#include <string>
#include <vector>

#include "eavp/media/queue.hpp"

namespace eavp {

template <typename T>
class InputPort;

template <typename T>
class OutputPort;

template <typename T>
Status connect(OutputPort<T>& output, InputPort<T>& input);

template <typename T>
class OutputPort {
public:
    explicit OutputPort(const std::string& name) : name_(name) {}

    Status send(const std::shared_ptr<const T>& value) {
        if (connections_.empty()) {
            return Status(StatusCode::kInvalidState, "output port is not connected");
        }
        for (typename std::vector<std::shared_ptr<BoundedQueue<T> > >::iterator it =
                 connections_.begin();
             it != connections_.end(); ++it) {
            const Status status = (*it)->preflight_push(value);
            if (!status.ok()) {
                return status;
            }
        }
        for (typename std::vector<std::shared_ptr<BoundedQueue<T> > >::iterator it =
                 connections_.begin();
             it != connections_.end(); ++it) {
            const Status status = (*it)->push(value);
            if (!status.ok()) {
                return status;
            }
        }
        return Status::ok_status();
    }

    const std::string& name() const { return name_; }

private:
    friend Status connect<T>(OutputPort<T>& output, InputPort<T>& input);

    std::string name_;
    std::vector<std::shared_ptr<BoundedQueue<T> > > connections_;
};

template <typename T>
class InputPort {
public:
    InputPort(const std::string& name, std::size_t capacity, OverflowPolicy policy)
        : name_(name), capacity_(capacity), policy_(policy) {}

    Result<std::shared_ptr<const T> > receive() {
        if (!queue_) {
            return Result<std::shared_ptr<const T> >(
                Status(StatusCode::kInvalidState, "input port is not connected"));
        }
        return queue_->pop();
    }

    std::size_t queue_size() const { return queue_ ? queue_->size() : 0U; }
    const std::string& name() const { return name_; }

private:
    friend Status connect<T>(OutputPort<T>& output, InputPort<T>& input);

    std::string name_;
    std::size_t capacity_;
    OverflowPolicy policy_;
    std::shared_ptr<BoundedQueue<T> > queue_;
};

template <typename T>
Status connect(OutputPort<T>& output, InputPort<T>& input) {
    if (input.queue_) {
        return Status(StatusCode::kAlreadyExists, "input port already has an upstream");
    }
    if (input.capacity_ == 0U) {
        return Status(StatusCode::kInvalidArgument, "input queue capacity must be positive");
    }
    input.queue_.reset(new BoundedQueue<T>(input.capacity_, input.policy_));
    output.connections_.push_back(input.queue_);
    return Status::ok_status();
}

}  // namespace eavp

#endif  // EAVP_MEDIA_PORT_HPP_
