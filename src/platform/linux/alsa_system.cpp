#include "platform/linux/alsa_system.hpp"

#include <cerrno>
#include <limits>

namespace eavp {
namespace detail {
namespace {

Status alsa_failure(StatusCode code, const char* operation, int native_code,
                    const AlsaApi& api) {
    return Status(code, api.error_string(native_code), "alsa", operation,
                  native_code);
}

StatusCode system_error_code(int native_code) {
    if (native_code == -ENOENT) return StatusCode::kNotFound;
    if (native_code == -ENODEV || native_code == -ENXIO) {
        return StatusCode::kDeviceLost;
    }
    if (native_code == -ENOMEM) return StatusCode::kResourceExhausted;
    return StatusCode::kIoError;
}

StatusCode capability_error_code(int native_code) {
    const StatusCode code = system_error_code(native_code);
    return code == StatusCode::kIoError ? StatusCode::kCapabilityMismatch : code;
}

snd_pcm_format_t alsa_format(SampleFormat format, bool* supported) {
    *supported = true;
    switch (format) {
        case SampleFormat::kSigned16LittleEndian:
            return SND_PCM_FORMAT_S16_LE;
        case SampleFormat::kSigned24In32LittleEndian:
            return SND_PCM_FORMAT_S24_LE;
        case SampleFormat::kSigned32LittleEndian:
            return SND_PCM_FORMAT_S32_LE;
        case SampleFormat::kFloat32LittleEndian:
            return SND_PCM_FORMAT_FLOAT_LE;
        case SampleFormat::kUnknown:
            *supported = false;
            return SND_PCM_FORMAT_UNKNOWN;
    }
    *supported = false;
    return SND_PCM_FORMAT_UNKNOWN;
}

}  // namespace

AlsaSystem::AlsaSystem(std::unique_ptr<AlsaApi> api)
    : api_(std::move(api)), pcm_(NULL), hw_params_(NULL), sw_params_(NULL),
      negotiated_(), state_(kCreated) {}

AlsaSystem::~AlsaSystem() noexcept {
    try {
        close_resources();
    } catch (...) {
    }
}

int AlsaSystem::close_resources() {
    int close_result = 0;
    if (sw_params_ != NULL) {
        api_->sw_params_free(sw_params_);
        sw_params_ = NULL;
    }
    if (hw_params_ != NULL) {
        api_->hw_params_free(hw_params_);
        hw_params_ = NULL;
    }
    if (pcm_ != NULL) {
        close_result = api_->pcm_close(pcm_);
        pcm_ = NULL;
    }
    negotiated_ = AlsaNegotiatedParameters();
    state_ = kCreated;
    return close_result;
}

Status AlsaSystem::prepare(const AlsaCaptureConfig& config) {
    if (!api_) {
        return Status(StatusCode::kInvalidState, "ALSA API is not configured");
    }
    if (state_ != kCreated) {
        return Status(StatusCode::kInvalidState, "ALSA device is already prepared");
    }

    bool supported = false;
    const snd_pcm_format_t format = alsa_format(config.format().sample_format(),
                                                &supported);
    if (!supported) {
        return Status(StatusCode::kCapabilityMismatch,
                      "ALSA sample format is not supported");
    }

    const int open_mode = SND_PCM_NONBLOCK | SND_PCM_NO_AUTO_RESAMPLE |
                          SND_PCM_NO_AUTO_CHANNELS | SND_PCM_NO_AUTO_FORMAT;
    int result = api_->pcm_open(&pcm_, config.device_name().c_str(),
                                SND_PCM_STREAM_CAPTURE, open_mode);
    if (result < 0) {
        close_resources();
        return alsa_failure(system_error_code(result), "snd_pcm_open", result, *api_);
    }

    result = api_->hw_params_malloc(&hw_params_);
    if (result < 0) {
        close_resources();
        return alsa_failure(system_error_code(result), "snd_pcm_hw_params_malloc",
                            result, *api_);
    }

#define EAVP_ALSA_PREPARE_CAPABILITY_CALL(expression, operation)                  \
    do {                                                                            \
        result = (expression);                                                      \
        if (result < 0) {                                                           \
            close_resources();                                                      \
            return alsa_failure(capability_error_code(result), operation, result, \
                                *api_);                                             \
        }                                                                           \
    } while (false)

    EAVP_ALSA_PREPARE_CAPABILITY_CALL(
        api_->hw_params_any(pcm_, hw_params_), "snd_pcm_hw_params_any");
    EAVP_ALSA_PREPARE_CAPABILITY_CALL(
        api_->hw_params_set_access(pcm_, hw_params_, SND_PCM_ACCESS_RW_INTERLEAVED),
        "snd_pcm_hw_params_set_access");
    EAVP_ALSA_PREPARE_CAPABILITY_CALL(
        api_->hw_params_set_format(pcm_, hw_params_, format),
        "snd_pcm_hw_params_set_format");
    EAVP_ALSA_PREPARE_CAPABILITY_CALL(
        api_->hw_params_set_channels(pcm_, hw_params_,
                                     static_cast<unsigned int>(config.format().channels())),
        "snd_pcm_hw_params_set_channels");
    EAVP_ALSA_PREPARE_CAPABILITY_CALL(
        api_->hw_params_set_rate(pcm_, hw_params_,
                                 static_cast<unsigned int>(config.format().sample_rate()), 0),
        "snd_pcm_hw_params_set_rate");

    snd_pcm_uframes_t period_frames =
        static_cast<snd_pcm_uframes_t>(config.period_size_hint());
    int direction = 0;
    EAVP_ALSA_PREPARE_CAPABILITY_CALL(
        api_->hw_params_set_period_size_near(pcm_, hw_params_, &period_frames,
                                              &direction),
        "snd_pcm_hw_params_set_period_size_near");
    if (period_frames == 0U ||
        period_frames > std::numeric_limits<snd_pcm_uframes_t>::max() /
                            static_cast<snd_pcm_uframes_t>(config.buffer_periods())) {
        close_resources();
        return Status(StatusCode::kCapabilityMismatch,
                      "ALSA buffer size negotiation overflows");
    }
    snd_pcm_uframes_t buffer_frames =
        period_frames * static_cast<snd_pcm_uframes_t>(config.buffer_periods());
    EAVP_ALSA_PREPARE_CAPABILITY_CALL(
        api_->hw_params_set_buffer_size_near(pcm_, hw_params_, &buffer_frames),
        "snd_pcm_hw_params_set_buffer_size_near");
    EAVP_ALSA_PREPARE_CAPABILITY_CALL(api_->hw_params(pcm_, hw_params_),
                                      "snd_pcm_hw_params");

    snd_pcm_access_t actual_access = SND_PCM_ACCESS_MMAP_INTERLEAVED;
    snd_pcm_format_t actual_format = SND_PCM_FORMAT_UNKNOWN;
    unsigned int actual_channels = 0U;
    unsigned int actual_rate = 0U;
    snd_pcm_uframes_t actual_period = 0U;
    snd_pcm_uframes_t actual_buffer = 0U;
    EAVP_ALSA_PREPARE_CAPABILITY_CALL(
        api_->hw_params_get_access(hw_params_, &actual_access),
        "snd_pcm_hw_params_get_access");
    EAVP_ALSA_PREPARE_CAPABILITY_CALL(
        api_->hw_params_get_format(hw_params_, &actual_format),
        "snd_pcm_hw_params_get_format");
    EAVP_ALSA_PREPARE_CAPABILITY_CALL(
        api_->hw_params_get_channels(hw_params_, &actual_channels),
        "snd_pcm_hw_params_get_channels");
    EAVP_ALSA_PREPARE_CAPABILITY_CALL(
        api_->hw_params_get_rate(hw_params_, &actual_rate, &direction),
        "snd_pcm_hw_params_get_rate");
    EAVP_ALSA_PREPARE_CAPABILITY_CALL(
        api_->hw_params_get_period_size(hw_params_, &actual_period, &direction),
        "snd_pcm_hw_params_get_period_size");
    EAVP_ALSA_PREPARE_CAPABILITY_CALL(
        api_->hw_params_get_buffer_size(hw_params_, &actual_buffer),
        "snd_pcm_hw_params_get_buffer_size");

    if (actual_access != SND_PCM_ACCESS_RW_INTERLEAVED || actual_format != format ||
        actual_channels != static_cast<unsigned int>(config.format().channels()) ||
        actual_rate != static_cast<unsigned int>(config.format().sample_rate()) ||
        actual_period == 0U || actual_buffer / actual_period < 2U) {
        close_resources();
        return Status(StatusCode::kCapabilityMismatch,
                      "ALSA negotiated parameters do not match the capture contract");
    }

    result = api_->sw_params_malloc(&sw_params_);
    if (result < 0) {
        close_resources();
        return alsa_failure(system_error_code(result), "snd_pcm_sw_params_malloc",
                            result, *api_);
    }
    EAVP_ALSA_PREPARE_CAPABILITY_CALL(
        api_->sw_params_current(pcm_, sw_params_), "snd_pcm_sw_params_current");
    EAVP_ALSA_PREPARE_CAPABILITY_CALL(
        api_->sw_params_set_tstamp_mode(pcm_, sw_params_, SND_PCM_TSTAMP_ENABLE),
        "snd_pcm_sw_params_set_tstamp_mode");

    const int timestamp_type_result = api_->sw_params_set_tstamp_type(
        pcm_, sw_params_, SND_PCM_TSTAMP_TYPE_MONOTONIC);
    const bool monotonic_timestamp = timestamp_type_result >= 0;
    if (timestamp_type_result < 0 && timestamp_type_result != -EINVAL) {
        close_resources();
        return alsa_failure(capability_error_code(timestamp_type_result),
                            "snd_pcm_sw_params_set_tstamp_type",
                            timestamp_type_result, *api_);
    }
    EAVP_ALSA_PREPARE_CAPABILITY_CALL(
        api_->sw_params_set_avail_min(pcm_, sw_params_, actual_period),
        "snd_pcm_sw_params_set_avail_min");
    EAVP_ALSA_PREPARE_CAPABILITY_CALL(api_->sw_params(pcm_, sw_params_),
                                      "snd_pcm_sw_params");

    result = api_->pcm_prepare(pcm_);
    if (result < 0) {
        close_resources();
        return alsa_failure(system_error_code(result), "snd_pcm_prepare", result,
                            *api_);
    }

#undef EAVP_ALSA_PREPARE_CAPABILITY_CALL

    negotiated_.sample_rate = actual_rate;
    negotiated_.channels = actual_channels;
    negotiated_.period_frames = actual_period;
    negotiated_.buffer_frames = actual_buffer;
    negotiated_.monotonic_timestamp = monotonic_timestamp;
    state_ = kPrepared;
    return Status::ok_status();
}

Status AlsaSystem::start() {
    if (state_ == kRunning) return Status::ok_status();
    if (state_ != kPrepared || pcm_ == NULL) {
        return Status(StatusCode::kInvalidState, "ALSA device is not prepared");
    }
    const int result = api_->pcm_start(pcm_);
    if (result < 0) {
        return alsa_failure(system_error_code(result), "snd_pcm_start", result,
                            *api_);
    }
    state_ = kRunning;
    return Status::ok_status();
}

Status AlsaSystem::stop() {
    if (state_ == kCreated || pcm_ == NULL) return Status::ok_status();

    const int drop_result = api_->pcm_drop(pcm_);
    const int close_result = close_resources();
    if (drop_result < 0) {
        return alsa_failure(system_error_code(drop_result), "snd_pcm_drop",
                            drop_result, *api_);
    }
    if (close_result < 0) {
        return alsa_failure(system_error_code(close_result), "snd_pcm_close",
                            close_result, *api_);
    }
    return Status::ok_status();
}

}  // namespace detail
}  // namespace eavp
