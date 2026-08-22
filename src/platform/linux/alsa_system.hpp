#ifndef EAVP_PLATFORM_LINUX_ALSA_SYSTEM_HPP_
#define EAVP_PLATFORM_LINUX_ALSA_SYSTEM_HPP_

#include <memory>

#include "eavp/base/status.hpp"
#include "eavp/platform/linux/alsa_capture.hpp"
#include "platform/linux/alsa_api.hpp"

namespace eavp {
namespace detail {

struct AlsaNegotiatedParameters {
    AlsaNegotiatedParameters()
        : sample_rate(0U), channels(0U), period_frames(0U), buffer_frames(0U),
          monotonic_timestamp(false) {}

    unsigned int sample_rate;
    unsigned int channels;
    snd_pcm_uframes_t period_frames;
    snd_pcm_uframes_t buffer_frames;
    bool monotonic_timestamp;
};

class AlsaSystem {
public:
    explicit AlsaSystem(std::unique_ptr<AlsaApi> api);
    ~AlsaSystem() noexcept;

    Status prepare(const AlsaCaptureConfig& config);
    Status start();
    Status stop();
    const AlsaNegotiatedParameters& negotiated() const { return negotiated_; }

private:
    enum State {
        kCreated,
        kPrepared,
        kRunning
    };

    int close_resources();

    std::unique_ptr<AlsaApi> api_;
    snd_pcm_t* pcm_;
    snd_pcm_hw_params_t* hw_params_;
    snd_pcm_sw_params_t* sw_params_;
    AlsaNegotiatedParameters negotiated_;
    State state_;
};

std::unique_ptr<AlsaApi> create_libasound_api();

}  // namespace detail
}  // namespace eavp

#endif  // EAVP_PLATFORM_LINUX_ALSA_SYSTEM_HPP_
