#ifndef EAVP_PLATFORM_LINUX_ALSA_CAPTURE_HPP_
#define EAVP_PLATFORM_LINUX_ALSA_CAPTURE_HPP_

#include <memory>
#include <string>

#include "eavp/base/result.hpp"
#include "eavp/media/audio_format.hpp"
#include "eavp/media/frame.hpp"
#include "eavp/media/node.hpp"
#include "eavp/media/port.hpp"

namespace eavp {

class MetricRegistry;
class HealthManager;

namespace detail {
class AlsaSourceNodeTestPeer;
}

class AlsaCaptureConfig {
public:
    static Result<AlsaCaptureConfig> create(const std::string& device_name,
                                            const AudioFormat& format,
                                            int samples_per_frame,
                                            int period_size_hint,
                                            int buffer_periods);

    const std::string& device_name() const { return device_name_; }
    const AudioFormat& format() const { return format_; }
    int samples_per_frame() const { return samples_per_frame_; }
    int period_size_hint() const { return period_size_hint_; }
    int buffer_periods() const { return buffer_periods_; }

private:
    AlsaCaptureConfig(const std::string& device_name, const AudioFormat& format,
                      int samples_per_frame, int period_size_hint, int buffer_periods)
        : device_name_(device_name), format_(format), samples_per_frame_(samples_per_frame),
          period_size_hint_(period_size_hint), buffer_periods_(buffer_periods) {}

    std::string device_name_;
    AudioFormat format_;
    int samples_per_frame_;
    int period_size_hint_;
    int buffer_periods_;
};

class AlsaSourceNode : public MediaNode {
public:
    static Result<std::unique_ptr<AlsaSourceNode> > create(
        const std::string& id, const AlsaCaptureConfig& config,
        MetricRegistry* metrics, HealthManager* health);
    ~AlsaSourceNode() noexcept;

    OutputPort<AudioFrame>& output();

protected:
    Status on_prepare() override;
    Status on_start() override;
    Status on_stop() override;
    Status on_reset() override;
    Status on_tick() override;

private:
    class Impl;

    AlsaSourceNode(const std::string& id, std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;

    friend class detail::AlsaSourceNodeTestPeer;
};

}  // namespace eavp

#endif  // EAVP_PLATFORM_LINUX_ALSA_CAPTURE_HPP_
