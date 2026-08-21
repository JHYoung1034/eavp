#include "eavp/management/health.hpp"

#include <new>

namespace eavp {

Status HealthManager::report(const std::string& component, HealthStatus status,
                             const std::string& message) {
    if (component.empty()) {
        return Status(StatusCode::kInvalidArgument, "health component must not be empty");
    }
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        std::map<std::string, HealthComponent>::iterator existing =
            components_.find(component);
        if (existing == components_.end()) {
            components_.insert(
                std::make_pair(component, HealthComponent(status, message)));
        } else {
            existing->second = HealthComponent(status, message);
        }
        return Status::ok_status();
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::kResourceExhausted);
    } catch (...) {
        return Status(StatusCode::kInternal);
    }
}

Result<HealthComponent> HealthManager::component(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::map<std::string, HealthComponent>::const_iterator value = components_.find(name);
    if (value == components_.end()) {
        return Result<HealthComponent>(
            Status(StatusCode::kNotFound, "health component was not found"));
    }
    return Result<HealthComponent>(value->second);
}

HealthStatus HealthManager::aggregate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    HealthStatus aggregate = HealthStatus::kOk;
    for (std::map<std::string, HealthComponent>::const_iterator it = components_.begin();
         it != components_.end(); ++it) {
        if (static_cast<int>(it->second.status) > static_cast<int>(aggregate)) {
            aggregate = it->second.status;
        }
    }
    return aggregate;
}

}  // namespace eavp
