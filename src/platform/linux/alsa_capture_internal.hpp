#ifndef EAVP_PLATFORM_LINUX_ALSA_CAPTURE_INTERNAL_HPP_
#define EAVP_PLATFORM_LINUX_ALSA_CAPTURE_INTERNAL_HPP_

#include <memory>
#include <string>

#include "eavp/platform/linux/alsa_capture.hpp"
#include "platform/linux/alsa_system.hpp"

namespace eavp {
namespace detail {

class AlsaSourceNodeTestPeer {
public:
    static Result<std::unique_ptr<AlsaSourceNode> > create(
        const std::string& id, const AlsaCaptureConfig& config,
        MetricRegistry* metrics, HealthManager* health,
        std::unique_ptr<AlsaSystem> system);
};

}  // namespace detail
}  // namespace eavp

#endif  // EAVP_PLATFORM_LINUX_ALSA_CAPTURE_INTERNAL_HPP_
