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

private:
    Status status_;
    std::shared_ptr<T> value_;
};

}  // namespace eavp

#endif  // EAVP_BASE_RESULT_HPP_
