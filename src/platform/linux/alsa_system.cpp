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

bool is_device_lost_error(int native_code) {
    return native_code == -ENODEV || native_code == -ENXIO;
}

StatusCode capability_error_code(int native_code) {
    const StatusCode code = system_error_code(native_code);
    return code == StatusCode::kIoError ? StatusCode::kCapabilityMismatch : code;
}

Status read_failure(snd_pcm_sframes_t native_code, const AlsaApi& api) {
    return alsa_failure(system_error_code(static_cast<int>(native_code)),
                        "snd_pcm_readi", static_cast<int>(native_code), api);
}

const std::int64_t kMicrosecondsPerSecond = 1000000;
const std::int64_t kNanosecondsPerSecond = 1000000000;
const std::int64_t kMaximumFutureTimestampUs = 1000000;

bool timespec_to_us(const struct timespec& value, std::int64_t* result) {
    if (result == NULL || value.tv_sec < 0 || value.tv_nsec < 0 ||
        value.tv_nsec >= kNanosecondsPerSecond ||
        value.tv_sec > std::numeric_limits<std::int64_t>::max() /
                           kMicrosecondsPerSecond) {
        return false;
    }
    const std::int64_t seconds_us =
        static_cast<std::int64_t>(value.tv_sec) * kMicrosecondsPerSecond;
    const std::int64_t nanoseconds_us =
        static_cast<std::int64_t>(value.tv_nsec) / 1000;
    if (seconds_us > std::numeric_limits<std::int64_t>::max() - nanoseconds_us) {
        return false;
    }
    *result = seconds_us + nanoseconds_us;
    return true;
}

bool frames_to_us(snd_pcm_uframes_t frames, unsigned int sample_rate,
                  std::int64_t* result) {
    if (result == NULL || sample_rate == 0U ||
        frames > static_cast<snd_pcm_uframes_t>(
                     std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    const std::int64_t frame_count = static_cast<std::int64_t>(frames);
    const std::int64_t quotient = frame_count / static_cast<std::int64_t>(sample_rate);
    const std::int64_t remainder = frame_count % static_cast<std::int64_t>(sample_rate);
    if (quotient > std::numeric_limits<std::int64_t>::max() /
                       kMicrosecondsPerSecond) {
        return false;
    }
    const std::int64_t seconds_us = quotient * kMicrosecondsPerSecond;
    if (remainder > std::numeric_limits<std::int64_t>::max() /
                        kMicrosecondsPerSecond) {
        return false;
    }
    const std::int64_t remainder_us =
        (remainder * kMicrosecondsPerSecond) / static_cast<std::int64_t>(sample_rate);
    if (seconds_us > std::numeric_limits<std::int64_t>::max() - remainder_us) {
        return false;
    }
    *result = seconds_us + remainder_us;
    return true;
}

bool first_unread_pts(std::int64_t timestamp_us, snd_pcm_uframes_t available,
                      unsigned int sample_rate, std::int64_t* result) {
    std::int64_t available_us = 0;
    if (!frames_to_us(available, sample_rate, &available_us) ||
        timestamp_us < available_us) {
        return false;
    }
    *result = timestamp_us - available_us;
    return true;
}

Status timestamp_failure() {
    return Status(StatusCode::kCorruptData,
                  "ALSA timestamp cannot produce a valid monotonic anchor");
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
      negotiated_(), state_(kCreated), suspended_(false) {}

AlsaSystem::~AlsaSystem() noexcept {
    try {
        close_resources();
    } catch (...) {
    }
}

AlsaSystem::AlsaSystem(AlsaSystem&& other) noexcept
    : api_(std::move(other.api_)), pcm_(other.pcm_), hw_params_(other.hw_params_),
      sw_params_(other.sw_params_), negotiated_(other.negotiated_),
      state_(other.state_), suspended_(other.suspended_) {
    other.pcm_ = NULL;
    other.hw_params_ = NULL;
    other.sw_params_ = NULL;
    other.negotiated_ = AlsaNegotiatedParameters();
    other.state_ = kCreated;
    other.suspended_ = false;
}

AlsaSystem& AlsaSystem::operator=(AlsaSystem&& other) noexcept {
    if (this != &other) {
        try {
            close_resources();
        } catch (...) {
        }
        api_ = std::move(other.api_);
        pcm_ = other.pcm_;
        hw_params_ = other.hw_params_;
        sw_params_ = other.sw_params_;
        negotiated_ = other.negotiated_;
        state_ = other.state_;
        suspended_ = other.suspended_;
        other.pcm_ = NULL;
        other.hw_params_ = NULL;
        other.sw_params_ = NULL;
        other.negotiated_ = AlsaNegotiatedParameters();
        other.state_ = kCreated;
        other.suspended_ = false;
    }
    return *this;
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
    suspended_ = false;
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

Status AlsaSystem::recover_xrun() {
    const int prepare_result = api_->pcm_prepare(pcm_);
    if (prepare_result < 0) {
        return alsa_failure(system_error_code(prepare_result),
                            "snd_pcm_prepare", prepare_result, *api_);
    }
    const int start_result = api_->pcm_start(pcm_);
    if (start_result < 0) {
        return alsa_failure(system_error_code(start_result),
                            "snd_pcm_start", start_result, *api_);
    }
    state_ = kRunning;
    return Status::ok_status();
}

void AlsaSystem::begin_suspend_recovery() {
    suspended_ = true;
}

Result<AlsaReadResult> AlsaSystem::read_interleaved(std::uint8_t* destination,
                                                    int requested_frames) {
    if (state_ != kRunning || pcm_ == NULL || destination == NULL ||
        requested_frames <= 0) {
        return Result<AlsaReadResult>(Status(
            StatusCode::kInvalidArgument, "ALSA read request is invalid"));
    }

    if (suspended_) {
        const int resume_result = api_->pcm_resume(pcm_);
        if (resume_result == -EAGAIN) {
            return Result<AlsaReadResult>(AlsaReadResult(0, true, false));
        }
        if (resume_result >= 0) {
            suspended_ = false;
            return Result<AlsaReadResult>(AlsaReadResult(0, true, false));
        }
        if (is_device_lost_error(resume_result)) {
            return Result<AlsaReadResult>(alsa_failure(
                StatusCode::kDeviceLost, "snd_pcm_resume", resume_result, *api_));
        }
        const int prepare_result = api_->pcm_prepare(pcm_);
        if (prepare_result < 0) {
            return Result<AlsaReadResult>(alsa_failure(
                system_error_code(prepare_result), "snd_pcm_prepare", prepare_result,
                *api_));
        }
        const int start_result = api_->pcm_start(pcm_);
        if (start_result < 0) {
            return Result<AlsaReadResult>(alsa_failure(
                system_error_code(start_result), "snd_pcm_start", start_result, *api_));
        }
        suspended_ = false;
        state_ = kRunning;
        return Result<AlsaReadResult>(AlsaReadResult(0, true, false));
    }

    snd_pcm_sframes_t result = 0;
    do {
        result = api_->pcm_readi(
            pcm_, destination, static_cast<snd_pcm_uframes_t>(requested_frames));
    } while (result == -EINTR);

    if (result > 0) {
        if (result > requested_frames ||
            result > std::numeric_limits<int>::max()) {
            return Result<AlsaReadResult>(Status(
                StatusCode::kCorruptData,
                "ALSA read returned an invalid frame count"));
        }
        return Result<AlsaReadResult>(AlsaReadResult(
            static_cast<int>(result), false, false));
    }
    if (result == 0 || result == -EAGAIN) {
        return Result<AlsaReadResult>(AlsaReadResult(0, true, false));
    }
    if (result == -EPIPE) {
        const Status recovery = recover_xrun();
        if (!recovery.ok()) return Result<AlsaReadResult>(recovery);
        return Result<AlsaReadResult>(AlsaReadResult(0, true, true));
    }
    if (result == -ESTRPIPE) {
        begin_suspend_recovery();
        return Result<AlsaReadResult>(AlsaReadResult(0, true, true));
    }
    return Result<AlsaReadResult>(read_failure(result, *api_));
}

Result<AlsaAnchor> AlsaSystem::capture_anchor() {
    if (state_ != kRunning || pcm_ == NULL || negotiated_.sample_rate == 0U) {
        return Result<AlsaAnchor>(Status(
            StatusCode::kInvalidState, "ALSA timestamp request is invalid"));
    }

    struct timespec now;
    const int monotonic_now_result = api_->monotonic_now(&now);
    if (monotonic_now_result < 0) {
        return Result<AlsaAnchor>(alsa_failure(
            system_error_code(monotonic_now_result), "clock_gettime(CLOCK_MONOTONIC)",
            monotonic_now_result, *api_));
    }
    std::int64_t now_us = 0;
    const bool valid_monotonic_now = timespec_to_us(now, &now_us);

    std::int64_t timestamp_us = 0;
    std::int64_t anchor_us = 0;
    if (negotiated_.monotonic_timestamp) {
        snd_pcm_uframes_t available = 0U;
        snd_htimestamp_t timestamp;
        const int htimestamp_result =
            api_->pcm_htimestamp(pcm_, &available, &timestamp);
        if (htimestamp_result == -EPIPE) {
            const Status recovery = recover_xrun();
            if (!recovery.ok()) return Result<AlsaAnchor>(recovery);
            return Result<AlsaAnchor>(
                AlsaAnchor(AlsaAnchor::kTimelineDiscontinuity));
        }
        if (htimestamp_result == -ESTRPIPE) {
            begin_suspend_recovery();
            return Result<AlsaAnchor>(
                AlsaAnchor(AlsaAnchor::kTimelineDiscontinuity));
        }
        if (is_device_lost_error(htimestamp_result)) {
            return Result<AlsaAnchor>(alsa_failure(
                StatusCode::kDeviceLost, "snd_pcm_htimestamp",
                htimestamp_result, *api_));
        }
        if (htimestamp_result >= 0 && valid_monotonic_now &&
            timespec_to_us(timestamp, &timestamp_us) &&
            !(timestamp_us > now_us &&
              timestamp_us - now_us > kMaximumFutureTimestampUs) &&
            first_unread_pts(timestamp_us, available, negotiated_.sample_rate,
                             &anchor_us)) {
            return Result<AlsaAnchor>(AlsaAnchor(anchor_us, false));
        }
    }

    const snd_pcm_sframes_t fallback_available = api_->pcm_avail_update(pcm_);
    if (fallback_available < 0) {
        if (fallback_available == -EAGAIN) {
            return Result<AlsaAnchor>(AlsaAnchor(AlsaAnchor::kWouldBlock));
        }
        if (fallback_available == -EPIPE) {
            const Status recovery = recover_xrun();
            if (!recovery.ok()) return Result<AlsaAnchor>(recovery);
            return Result<AlsaAnchor>(
                AlsaAnchor(AlsaAnchor::kTimelineDiscontinuity));
        }
        if (fallback_available == -ESTRPIPE) {
            begin_suspend_recovery();
            return Result<AlsaAnchor>(
                AlsaAnchor(AlsaAnchor::kTimelineDiscontinuity));
        }
        return Result<AlsaAnchor>(alsa_failure(
            system_error_code(static_cast<int>(fallback_available)),
            "snd_pcm_avail_update", static_cast<int>(fallback_available), *api_));
    }
    if (!valid_monotonic_now ||
        !first_unread_pts(now_us,
                          static_cast<snd_pcm_uframes_t>(fallback_available),
                          negotiated_.sample_rate, &anchor_us)) {
        return Result<AlsaAnchor>(timestamp_failure());
    }
    return Result<AlsaAnchor>(AlsaAnchor(anchor_us, true));
}

}  // namespace detail
}  // namespace eavp
