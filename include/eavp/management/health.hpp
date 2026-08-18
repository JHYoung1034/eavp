#ifndef EAVP_MANAGEMENT_HEALTH_HPP_
#define EAVP_MANAGEMENT_HEALTH_HPP_

#include <map>
#include <mutex>
#include <string>

#include "eavp/base/result.hpp"

namespace eavp {

enum class HealthStatus {
    kOk = 0,
    kDegraded = 1,
    kError = 2,
    kCritical = 3,
};

struct HealthComponent {
    HealthComponent(HealthStatus component_status, const std::string& component_message)
        : status(component_status), message(component_message) {}

    HealthStatus status;
    std::string message;
};

class HealthManager {
public:
    Status report(const std::string& component, HealthStatus status,
                  const std::string& message);
    Result<HealthComponent> component(const std::string& name) const;
    HealthStatus aggregate() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, HealthComponent> components_;
};

}  // namespace eavp

#endif  // EAVP_MANAGEMENT_HEALTH_HPP_

