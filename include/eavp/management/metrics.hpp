#ifndef EAVP_MANAGEMENT_METRICS_HPP_
#define EAVP_MANAGEMENT_METRICS_HPP_

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "eavp/base/result.hpp"

namespace eavp {

class MetricRegistry {
public:
    Status increment_counter(const std::string& name, std::uint64_t delta = 1U);
    Status set_gauge(const std::string& name, double value);

    Result<std::uint64_t> counter(const std::string& name) const;
    Result<double> gauge(const std::string& name) const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::uint64_t> counters_;
    std::map<std::string, double> gauges_;
};

}  // namespace eavp

#endif  // EAVP_MANAGEMENT_METRICS_HPP_

