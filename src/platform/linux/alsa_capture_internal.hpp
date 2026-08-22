#ifndef EAVP_PLATFORM_LINUX_ALSA_CAPTURE_INTERNAL_HPP_
#define EAVP_PLATFORM_LINUX_ALSA_CAPTURE_INTERNAL_HPP_

#include <memory>
#include <string>

#include "eavp/platform/linux/alsa_capture.hpp"
#include "platform/linux/alsa_system.hpp"

namespace eavp {
namespace detail {

class AlsaObserver {
public:
    virtual ~AlsaObserver() {}
    virtual Status on_negotiated(int period_frames, int buffer_frames) = 0;
    virtual Status on_partial(int partial_samples) = 0;
    virtual Status on_would_block() = 0;
    virtual Status on_frame(const AudioFrame& frame) = 0;
    virtual Status on_timestamp_fallback() = 0;
    virtual Status on_recovery(bool xrun) = 0;
    virtual Status on_fatal(const Status& failure) = 0;
};

class AlsaSourceNodeTestPeer {
public:
    static Result<std::unique_ptr<AlsaSourceNode> > create(
        const std::string& id, const AlsaCaptureConfig& config,
        MetricRegistry* metrics, HealthManager* health,
        std::unique_ptr<AlsaSystem> system,
        // observer 仅由测试调用方持有，必须在 Node 销毁后再销毁。
        AlsaObserver* observer = NULL);
};

}  // namespace detail
}  // namespace eavp

#endif  // EAVP_PLATFORM_LINUX_ALSA_CAPTURE_INTERNAL_HPP_
