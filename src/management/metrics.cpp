#include "eavp/management/metrics.hpp"

namespace eavp {

Status MetricRegistry::increment_counter(const std::string& name, std::uint64_t delta) {
    if (name.empty()) {
        return Status(StatusCode::kInvalidArgument, "metric name must not be empty");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    counters_[name] += delta;
    return Status::ok_status();
}

Status MetricRegistry::set_gauge(const std::string& name, double value) {
    if (name.empty()) {
        return Status(StatusCode::kInvalidArgument, "metric name must not be empty");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    gauges_[name] = value;
    return Status::ok_status();
}

Result<std::uint64_t> MetricRegistry::counter(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::map<std::string, std::uint64_t>::const_iterator value = counters_.find(name);
    if (value == counters_.end()) {
        return Result<std::uint64_t>(Status(StatusCode::kNotFound, "counter was not found"));
    }
    return Result<std::uint64_t>(value->second);
}

Result<double> MetricRegistry::gauge(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::map<std::string, double>::const_iterator value = gauges_.find(name);
    if (value == gauges_.end()) {
        return Result<double>(Status(StatusCode::kNotFound, "gauge was not found"));
    }
    return Result<double>(value->second);
}

}  // namespace eavp

