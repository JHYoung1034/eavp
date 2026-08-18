#include "eavp/control/state_store.hpp"

namespace eavp {

namespace {

Status type_error() {
    return Status(StatusCode::kInvalidState, "state value has a different type");
}

}  // namespace

StateValue::StateValue(bool value)
    : type_(StateValueType::kBool),
      bool_value_(value),
      int_value_(0),
      double_value_(0.0) {}

StateValue::StateValue(std::int64_t value)
    : type_(StateValueType::kInt64),
      bool_value_(false),
      int_value_(value),
      double_value_(0.0) {}

StateValue::StateValue(double value)
    : type_(StateValueType::kDouble),
      bool_value_(false),
      int_value_(0),
      double_value_(value) {}

StateValue::StateValue(const std::string& value)
    : type_(StateValueType::kString),
      bool_value_(false),
      int_value_(0),
      double_value_(0.0),
      string_value_(value) {}

StateValue::StateValue(const char* value) : StateValue(std::string(value == NULL ? "" : value)) {}

StateValueType StateValue::type() const { return type_; }

Result<bool> StateValue::as_bool() const {
    return type_ == StateValueType::kBool ? Result<bool>(bool_value_)
                                          : Result<bool>(type_error());
}

Result<std::int64_t> StateValue::as_int64() const {
    return type_ == StateValueType::kInt64 ? Result<std::int64_t>(int_value_)
                                           : Result<std::int64_t>(type_error());
}

Result<double> StateValue::as_double() const {
    return type_ == StateValueType::kDouble ? Result<double>(double_value_)
                                            : Result<double>(type_error());
}

Result<std::string> StateValue::as_string() const {
    return type_ == StateValueType::kString ? Result<std::string>(string_value_)
                                            : Result<std::string>(type_error());
}

bool StateValue::operator==(const StateValue& other) const {
    if (type_ != other.type_) {
        return false;
    }
    switch (type_) {
        case StateValueType::kBool:
            return bool_value_ == other.bool_value_;
        case StateValueType::kInt64:
            return int_value_ == other.int_value_;
        case StateValueType::kDouble:
            return double_value_ == other.double_value_;
        case StateValueType::kString:
            return string_value_ == other.string_value_;
    }
    return false;
}

bool StateValue::operator!=(const StateValue& other) const { return !(*this == other); }

StateSnapshot::StateSnapshot(std::uint64_t version,
                             const std::map<std::string, StateValue>& values)
    : version_(version), values_(values) {}

std::uint64_t StateSnapshot::version() const { return version_; }

Result<StateValue> StateSnapshot::get(const std::string& key) const {
    const std::map<std::string, StateValue>::const_iterator value = values_.find(key);
    if (value == values_.end()) {
        return Result<StateValue>(Status(StatusCode::kNotFound, "state key was not found"));
    }
    return Result<StateValue>(value->second);
}

StateStore::StateStore() : version_(0U) {}

Status StateStore::set(const std::string& key, const StateValue& value) {
    if (key.empty() || key[0] != '/') {
        return Status(StatusCode::kInvalidArgument, "state key must be an absolute path");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, StateValue>::iterator existing = values_.find(key);
    if (existing != values_.end() && existing->second == value) {
        return Status::ok_status();
    }
    if (existing == values_.end()) {
        values_.insert(std::make_pair(key, value));
    } else {
        existing->second = value;
    }
    ++version_;
    return Status::ok_status();
}

std::uint64_t StateStore::version() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return version_;
}

StateSnapshot StateStore::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return StateSnapshot(version_, values_);
}

}  // namespace eavp

