#ifndef EAVP_BASE_RESULT_HPP_
#define EAVP_BASE_RESULT_HPP_

#include <cassert>
#include <memory>
#include <utility>

#include "eavp/base/status.hpp"

namespace eavp {

template <typename T>
class Result {
public:
    Result(const T& value) : status_(), value_(new T(value)) {}
    Result(T&& value) : status_(), value_(new T(std::move(value))) {}

    Result(Result&& other) noexcept
        : status_(std::move(other.status_)), value_(std::move(other.value_)) {
        other.status_ = Status(StatusCode::kInvalidState, "result has been moved");
    }

    Result& operator=(Result&& other) noexcept {
        if (this != &other) {
            status_ = std::move(other.status_);
            value_ = std::move(other.value_);
            other.status_ = Status(StatusCode::kInvalidState, "result has been moved");
        }
        return *this;
    }
    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;

    explicit Result(const Status& status)
        : status_(status.ok()
                      ? Status(StatusCode::kInvalidArgument,
                               "a result without a value must contain a failure status")
                      : status) {}

    bool ok() const { return status_.ok(); }
    const Status& status() const { return status_; }

    const T& value() const {
        assert(ok());
        return *value_;
    }

    T& value() {
        assert(ok());
        return *value_;
    }

    T take_value() {
        assert(ok());
        return std::move(*value_);
    }

private:
    Status status_;
    std::unique_ptr<T> value_;
};

}  // namespace eavp

#endif  // EAVP_BASE_RESULT_HPP_
