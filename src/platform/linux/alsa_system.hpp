#ifndef EAVP_PLATFORM_LINUX_ALSA_SYSTEM_HPP_
#define EAVP_PLATFORM_LINUX_ALSA_SYSTEM_HPP_

#include <cstdint>
#include <memory>

#include "eavp/base/result.hpp"
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

struct AlsaReadResult {
    AlsaReadResult(int frames_read_value, bool would_block_value,
                   bool timeline_discontinuity_value)
        : frames_read(frames_read_value), would_block(would_block_value),
          timeline_discontinuity(timeline_discontinuity_value) {}

    int frames_read;
    bool would_block;
    bool timeline_discontinuity;
};

struct AlsaAnchor {
    enum Outcome {
        kAnchor,
        kWouldBlock,
        kTimelineDiscontinuity
    };

    explicit AlsaAnchor(Outcome outcome_value)
        : outcome(outcome_value), first_unread_pts_us(0), used_fallback(false) {}
    AlsaAnchor(std::int64_t first_unread_pts_us_value, bool used_fallback_value)
        : outcome(kAnchor), first_unread_pts_us(first_unread_pts_us_value),
          used_fallback(used_fallback_value) {}

    Outcome outcome;
    std::int64_t first_unread_pts_us;
    bool used_fallback;
};

class AlsaSystem {
public:
    explicit AlsaSystem(std::unique_ptr<AlsaApi> api);
    ~AlsaSystem() noexcept;
    AlsaSystem(AlsaSystem&& other) noexcept;
    AlsaSystem& operator=(AlsaSystem&& other) noexcept;
    AlsaSystem(const AlsaSystem&) = delete;
    AlsaSystem& operator=(const AlsaSystem&) = delete;

    Status prepare(const AlsaCaptureConfig& config);
    Status start();
    Status stop();
    Result<AlsaReadResult> read_interleaved(std::uint8_t* destination,
                                            int requested_frames);
    Result<AlsaAnchor> capture_anchor();
    const AlsaNegotiatedParameters& negotiated() const { return negotiated_; }
    bool suspend_recovery_pending() const { return suspended_; }

private:
    enum State {
        kCreated,
        kPrepared,
        kRunning
    };

    int close_resources();
    Status recover_xrun();
    void begin_suspend_recovery();

    std::unique_ptr<AlsaApi> api_;
    snd_pcm_t* pcm_;
    snd_pcm_hw_params_t* hw_params_;
    snd_pcm_sw_params_t* sw_params_;
    AlsaNegotiatedParameters negotiated_;
    State state_;
    bool suspended_;
};

std::unique_ptr<AlsaApi> create_libasound_api();

}  // namespace detail
}  // namespace eavp

#endif  // EAVP_PLATFORM_LINUX_ALSA_SYSTEM_HPP_
