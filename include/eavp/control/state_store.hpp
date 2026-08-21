#ifndef EAVP_CONTROL_STATE_STORE_HPP_
#define EAVP_CONTROL_STATE_STORE_HPP_

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "eavp/base/result.hpp"

namespace eavp {

enum class StateValueType {
    kBool,
    kInt64,
    kDouble,
    kString,
};

class StateValue {
public:
    explicit StateValue(bool value);
    explicit StateValue(std::int64_t value);
    explicit StateValue(double value);
    explicit StateValue(const std::string& value);
    explicit StateValue(const char* value);

    StateValueType type() const;
    Result<bool> as_bool() const;
    Result<std::int64_t> as_int64() const;
    Result<double> as_double() const;
    Result<std::string> as_string() const;

    bool operator==(const StateValue& other) const;
    bool operator!=(const StateValue& other) const;

private:
    StateValueType type_;
    bool bool_value_;
    std::int64_t int_value_;
    double double_value_;
    std::string string_value_;
};

class StateSnapshot {
public:
    StateSnapshot(std::uint64_t version, const std::map<std::string, StateValue>& values);

    std::uint64_t version() const;
    Result<StateValue> get(const std::string& key) const;

private:
    std::uint64_t version_;
    std::map<std::string, StateValue> values_;
};

class StateStore {
public:
    StateStore();

    Status set(const std::string& key, const StateValue& value);
    Status erase(const std::string& key);
    std::uint64_t version() const;
    StateSnapshot snapshot() const;

private:
    mutable std::mutex mutex_;
    std::uint64_t version_;
    std::map<std::string, StateValue> values_;
};

}  // namespace eavp

#endif  // EAVP_CONTROL_STATE_STORE_HPP_
