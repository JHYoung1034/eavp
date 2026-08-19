#ifndef EAVP_BASE_STATUS_HPP_
#define EAVP_BASE_STATUS_HPP_

#include <cstdint>
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
    kEndOfStream,
    kDeviceLost,
    kCorruptData,
};

class Status {
public:
    Status()
        : code_(StatusCode::kOk), native_code_(0), has_native_code_(false) {}

    Status(StatusCode code, const std::string& message)
        : code_(code), message_(message), native_code_(0), has_native_code_(false) {}

    Status(StatusCode code, const std::string& message, const std::string& provider_id,
           const std::string& operation, std::int64_t native_code)
        : code_(code),
          message_(message),
          provider_id_(provider_id),
          operation_(operation),
          native_code_(native_code),
          has_native_code_(true) {}

    static Status ok_status() { return Status(); }

    bool ok() const { return code_ == StatusCode::kOk; }
    StatusCode code() const { return code_; }
    const std::string& message() const { return message_; }
    const std::string& provider_id() const { return provider_id_; }
    const std::string& operation() const { return operation_; }
    bool has_native_code() const { return has_native_code_; }
    std::int64_t native_code() const { return native_code_; }

private:
    StatusCode code_;
    std::string message_;
    std::string provider_id_;
    std::string operation_;
    std::int64_t native_code_;
    bool has_native_code_;
};

}  // namespace eavp

#endif  // EAVP_BASE_STATUS_HPP_
