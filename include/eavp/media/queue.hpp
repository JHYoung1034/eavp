#ifndef EAVP_MEDIA_QUEUE_HPP_
#define EAVP_MEDIA_QUEUE_HPP_

#include <cstddef>
#include <deque>
#include <memory>

#include "eavp/base/result.hpp"

namespace eavp {

enum class OverflowPolicy {
    kBlock,
    kDropOldest,
    kDropNewest,
};

template <typename T>
class BoundedQueue {
public:
    BoundedQueue(std::size_t capacity, OverflowPolicy policy)
        : capacity_(capacity), policy_(policy), dropped_count_(0U) {}

    Status push(const std::shared_ptr<const T>& value) {
        if (!value) {
            return Status(StatusCode::kInvalidArgument, "queue value must not be null");
        }
        if (capacity_ == 0U) {
            return Status(StatusCode::kInvalidState, "queue capacity must be positive");
        }
        if (values_.size() == capacity_) {
            if (policy_ == OverflowPolicy::kBlock) {
                return Status(StatusCode::kWouldBlock, "queue is full");
            }
            if (policy_ == OverflowPolicy::kDropNewest) {
                ++dropped_count_;
                return Status::ok_status();
            }
            values_.pop_front();
            ++dropped_count_;
        }
        values_.push_back(value);
        return Status::ok_status();
    }

    Result<std::shared_ptr<const T> > pop() {
        if (values_.empty()) {
            return Result<std::shared_ptr<const T> >(
                Status(StatusCode::kNotFound, "queue is empty"));
        }
        const std::shared_ptr<const T> value = values_.front();
        values_.pop_front();
        return Result<std::shared_ptr<const T> >(value);
    }

    std::size_t size() const { return values_.size(); }
    std::size_t capacity() const { return capacity_; }
    std::size_t dropped_count() const { return dropped_count_; }

private:
    std::size_t capacity_;
    OverflowPolicy policy_;
    std::size_t dropped_count_;
    std::deque<std::shared_ptr<const T> > values_;
};

}  // namespace eavp

#endif  // EAVP_MEDIA_QUEUE_HPP_

