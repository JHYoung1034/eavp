#ifndef EAVP_BASE_STATUS_HPP_
#define EAVP_BASE_STATUS_HPP_

#include <string>

namespace eavp {

enum class StatusCode {
    kOk = 0,
    kInvalidArgument,
    kInvalidState,
    kNotFound,
    kAlreadyExists,
    kUnsupported,
    kCapabilityMismatch,
    kResourceExhausted,
    kWouldBlock,
    kTimeout,
    kIoError,
    kInternal,
};

class Status {
public:
    Status() : code_(StatusCode::kOk) {}

    Status(StatusCode code, const std::string& message) : code_(code), message_(message) {}

    static Status ok_status() { return Status(); }

    bool ok() const { return code_ == StatusCode::kOk; }
    StatusCode code() const { return code_; }
    const std::string& message() const { return message_; }

private:
    StatusCode code_;
    std::string message_;
};

}  // namespace eavp

#endif  // EAVP_BASE_STATUS_HPP_

