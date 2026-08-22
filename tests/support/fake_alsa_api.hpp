#ifndef EAVP_TESTS_SUPPORT_FAKE_ALSA_API_HPP_
#define EAVP_TESTS_SUPPORT_FAKE_ALSA_API_HPP_

#include <cerrno>
#include <ctime>

#include "platform/linux/alsa_api.hpp"

namespace eavp_test {

class FakeAlsaApi : public eavp::detail::AlsaApi {
public:
    enum Step {
        kOpen = 1,
        kHwParamsMalloc,
        kHwParamsAny,
        kHwParamsSetAccess,
        kHwParamsSetFormat,
        kHwParamsSetChannels,
        kHwParamsSetRate,
        kHwParamsSetPeriodSizeNear,
        kHwParamsSetBufferSizeNear,
        kHwParams,
        kHwParamsGetAccess,
        kHwParamsGetFormat,
        kHwParamsGetChannels,
        kHwParamsGetRate,
        kHwParamsGetPeriodSize,
        kHwParamsGetBufferSize,
        kSwParamsMalloc,
        kSwParamsCurrent,
        kSwParamsSetTstampMode,
        kSwParamsSetTstampType,
        kSwParamsSetAvailMin,
        kSwParams,
        kPcmPrepare,
        kPcmStart,
        kPcmDrop,
        kPcmClose
    };

    FakeAlsaApi()
        : fail_step(0), fail_code(-EIO), opened_stream(SND_PCM_STREAM_PLAYBACK),
          opened_mode(0), requested_access(SND_PCM_ACCESS_MMAP_INTERLEAVED),
          requested_format(SND_PCM_FORMAT_UNKNOWN), requested_channels(0U),
          requested_rate(0U), requested_period(0U), requested_buffer(0U),
          requested_avail_min(0U), negotiated_access(SND_PCM_ACCESS_RW_INTERLEAVED),
          negotiated_format(SND_PCM_FORMAT_S16_LE), negotiated_channels(2U),
          negotiated_rate(48000U), negotiated_period(480U),
          negotiated_buffer(1920U), reported_period(0U),
          accepts_requested_format(true),
          successful_open_count(0), close_count(0),
          hw_params_alloc_count(0), hw_params_free_count(0),
          sw_params_alloc_count(0), sw_params_free_count(0), pcm_start_count(0),
          pcm_drop_count(0), htimestamp_count(0), monotonic_now_count(0) {}

    int pcm_open(snd_pcm_t** pcm, const char*, snd_pcm_stream_t stream,
                 int mode) {
        opened_stream = stream;
        opened_mode = mode;
        if (fails(kOpen)) return fail_code;
        *pcm = reinterpret_cast<snd_pcm_t*>(this);
        ++successful_open_count;
        return 0;
    }
    int pcm_close(snd_pcm_t*) {
        ++close_count;
        return fails(kPcmClose) ? fail_code : 0;
    }
    int hw_params_malloc(snd_pcm_hw_params_t** params) {
        if (fails(kHwParamsMalloc)) return fail_code;
        *params = reinterpret_cast<snd_pcm_hw_params_t*>(this);
        ++hw_params_alloc_count;
        return 0;
    }
    void hw_params_free(snd_pcm_hw_params_t*) { ++hw_params_free_count; }
    int hw_params_any(snd_pcm_t*, snd_pcm_hw_params_t*) {
        return result(kHwParamsAny);
    }
    int hw_params_set_access(snd_pcm_t*, snd_pcm_hw_params_t*,
                             snd_pcm_access_t access) {
        requested_access = access;
        return result(kHwParamsSetAccess);
    }
    int hw_params_set_format(snd_pcm_t*, snd_pcm_hw_params_t*,
                             snd_pcm_format_t format) {
        requested_format = format;
        if (accepts_requested_format) negotiated_format = format;
        return result(kHwParamsSetFormat);
    }
    int hw_params_set_channels(snd_pcm_t*, snd_pcm_hw_params_t*,
                               unsigned int channels) {
        requested_channels = channels;
        return result(kHwParamsSetChannels);
    }
    int hw_params_set_rate(snd_pcm_t*, snd_pcm_hw_params_t*, unsigned int rate,
                           int) {
        requested_rate = rate;
        return result(kHwParamsSetRate);
    }
    int hw_params_set_period_size_near(snd_pcm_t*, snd_pcm_hw_params_t*,
                                       snd_pcm_uframes_t* frames, int*) {
        requested_period = *frames;
        *frames = negotiated_period;
        return result(kHwParamsSetPeriodSizeNear);
    }
    int hw_params_set_buffer_size_near(snd_pcm_t*, snd_pcm_hw_params_t*,
                                       snd_pcm_uframes_t* frames) {
        requested_buffer = *frames;
        *frames = negotiated_buffer;
        return result(kHwParamsSetBufferSizeNear);
    }
    int hw_params(snd_pcm_t*, snd_pcm_hw_params_t*) { return result(kHwParams); }
    int hw_params_get_access(const snd_pcm_hw_params_t*, snd_pcm_access_t* value) {
        *value = negotiated_access;
        return result(kHwParamsGetAccess);
    }
    int hw_params_get_format(const snd_pcm_hw_params_t*, snd_pcm_format_t* value) {
        *value = negotiated_format;
        return result(kHwParamsGetFormat);
    }
    int hw_params_get_channels(const snd_pcm_hw_params_t*, unsigned int* value) {
        *value = negotiated_channels;
        return result(kHwParamsGetChannels);
    }
    int hw_params_get_rate(const snd_pcm_hw_params_t*, unsigned int* value, int*) {
        *value = negotiated_rate;
        return result(kHwParamsGetRate);
    }
    int hw_params_get_period_size(const snd_pcm_hw_params_t*,
                                  snd_pcm_uframes_t* value, int*) {
        *value = reported_period == 0U ? negotiated_period : reported_period;
        return result(kHwParamsGetPeriodSize);
    }
    int hw_params_get_buffer_size(const snd_pcm_hw_params_t*,
                                  snd_pcm_uframes_t* value) {
        *value = negotiated_buffer;
        return result(kHwParamsGetBufferSize);
    }
    int sw_params_malloc(snd_pcm_sw_params_t** params) {
        if (fails(kSwParamsMalloc)) return fail_code;
        *params = reinterpret_cast<snd_pcm_sw_params_t*>(this);
        ++sw_params_alloc_count;
        return 0;
    }
    void sw_params_free(snd_pcm_sw_params_t*) { ++sw_params_free_count; }
    int sw_params_current(snd_pcm_t*, snd_pcm_sw_params_t*) {
        return result(kSwParamsCurrent);
    }
    int sw_params_set_tstamp_mode(snd_pcm_t*, snd_pcm_sw_params_t*,
                                  snd_pcm_tstamp_t) {
        return result(kSwParamsSetTstampMode);
    }
    int sw_params_set_tstamp_type(snd_pcm_t*, snd_pcm_sw_params_t*,
                                  snd_pcm_tstamp_type_t) {
        return result(kSwParamsSetTstampType);
    }
    int sw_params_set_avail_min(snd_pcm_t*, snd_pcm_sw_params_t*,
                                snd_pcm_uframes_t frames) {
        requested_avail_min = frames;
        return result(kSwParamsSetAvailMin);
    }
    int sw_params(snd_pcm_t*, snd_pcm_sw_params_t*) { return result(kSwParams); }
    int pcm_prepare(snd_pcm_t*) { return result(kPcmPrepare); }
    int pcm_start(snd_pcm_t*) {
        ++pcm_start_count;
        return result(kPcmStart);
    }
    int pcm_drop(snd_pcm_t*) {
        ++pcm_drop_count;
        return result(kPcmDrop);
    }
    int pcm_resume(snd_pcm_t*) { return 0; }
    snd_pcm_sframes_t pcm_readi(snd_pcm_t*, void*, snd_pcm_uframes_t) { return 0; }
    int pcm_htimestamp(snd_pcm_t*, snd_pcm_uframes_t*, snd_htimestamp_t*) {
        ++htimestamp_count;
        return 0;
    }
    snd_pcm_sframes_t pcm_avail_update(snd_pcm_t*) { return 0; }
    int monotonic_now(struct timespec*) {
        ++monotonic_now_count;
        return 0;
    }
    const char* error_string(int) const { return "fake ALSA failure"; }

    int fail_step;
    int fail_code;
    snd_pcm_stream_t opened_stream;
    int opened_mode;
    snd_pcm_access_t requested_access;
    snd_pcm_format_t requested_format;
    unsigned int requested_channels;
    unsigned int requested_rate;
    snd_pcm_uframes_t requested_period;
    snd_pcm_uframes_t requested_buffer;
    snd_pcm_uframes_t requested_avail_min;
    snd_pcm_access_t negotiated_access;
    snd_pcm_format_t negotiated_format;
    unsigned int negotiated_channels;
    unsigned int negotiated_rate;
    snd_pcm_uframes_t negotiated_period;
    snd_pcm_uframes_t negotiated_buffer;
    snd_pcm_uframes_t reported_period;
    bool accepts_requested_format;
    int successful_open_count;
    int close_count;
    int hw_params_alloc_count;
    int hw_params_free_count;
    int sw_params_alloc_count;
    int sw_params_free_count;
    int pcm_start_count;
    int pcm_drop_count;
    int htimestamp_count;
    int monotonic_now_count;

private:
    bool fails(Step step) const { return fail_step == static_cast<int>(step); }
    int result(Step step) const { return fails(step) ? fail_code : 0; }
};

}  // namespace eavp_test

#endif  // EAVP_TESTS_SUPPORT_FAKE_ALSA_API_HPP_
