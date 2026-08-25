#include "platform/linux/alsa_system.hpp"

#include <ctime>
#include <memory>

namespace eavp {
namespace detail {
namespace {

class LibasoundApi : public AlsaApi {
public:
    int pcm_open(snd_pcm_t** pcm, const char* name, snd_pcm_stream_t stream,
                 int mode) {
        return ::snd_pcm_open(pcm, name, stream, mode);
    }
    int pcm_close(snd_pcm_t* pcm) { return ::snd_pcm_close(pcm); }
    int hw_params_malloc(snd_pcm_hw_params_t** params) {
        return ::snd_pcm_hw_params_malloc(params);
    }
    void hw_params_free(snd_pcm_hw_params_t* params) {
        ::snd_pcm_hw_params_free(params);
    }
    int hw_params_any(snd_pcm_t* pcm, snd_pcm_hw_params_t* params) {
        return ::snd_pcm_hw_params_any(pcm, params);
    }
    int hw_params_set_access(snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
                             snd_pcm_access_t access) {
        return ::snd_pcm_hw_params_set_access(pcm, params, access);
    }
    int hw_params_set_format(snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
                             snd_pcm_format_t format) {
        return ::snd_pcm_hw_params_set_format(pcm, params, format);
    }
    int hw_params_set_channels(snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
                               unsigned int channels) {
        return ::snd_pcm_hw_params_set_channels(pcm, params, channels);
    }
    int hw_params_set_rate(snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
                           unsigned int rate, int direction) {
        return ::snd_pcm_hw_params_set_rate(pcm, params, rate, direction);
    }
    int hw_params_set_period_size_near(snd_pcm_t* pcm,
                                       snd_pcm_hw_params_t* params,
                                       snd_pcm_uframes_t* frames,
                                       int* direction) {
        return ::snd_pcm_hw_params_set_period_size_near(pcm, params, frames,
                                                        direction);
    }
    int hw_params_set_buffer_size_near(snd_pcm_t* pcm,
                                       snd_pcm_hw_params_t* params,
                                       snd_pcm_uframes_t* frames) {
        return ::snd_pcm_hw_params_set_buffer_size_near(pcm, params, frames);
    }
    int hw_params(snd_pcm_t* pcm, snd_pcm_hw_params_t* params) {
        return ::snd_pcm_hw_params(pcm, params);
    }
    int hw_params_get_access(const snd_pcm_hw_params_t* params,
                             snd_pcm_access_t* access) {
        return ::snd_pcm_hw_params_get_access(params, access);
    }
    int hw_params_get_format(const snd_pcm_hw_params_t* params,
                             snd_pcm_format_t* format) {
        return ::snd_pcm_hw_params_get_format(params, format);
    }
    int hw_params_get_channels(const snd_pcm_hw_params_t* params,
                               unsigned int* channels) {
        return ::snd_pcm_hw_params_get_channels(params, channels);
    }
    int hw_params_get_rate(const snd_pcm_hw_params_t* params,
                           unsigned int* rate, int* direction) {
        return ::snd_pcm_hw_params_get_rate(params, rate, direction);
    }
    int hw_params_get_period_size(const snd_pcm_hw_params_t* params,
                                  snd_pcm_uframes_t* frames, int* direction) {
        return ::snd_pcm_hw_params_get_period_size(params, frames, direction);
    }
    int hw_params_get_buffer_size(const snd_pcm_hw_params_t* params,
                                  snd_pcm_uframes_t* frames) {
        return ::snd_pcm_hw_params_get_buffer_size(params, frames);
    }
    int sw_params_malloc(snd_pcm_sw_params_t** params) {
        return ::snd_pcm_sw_params_malloc(params);
    }
    void sw_params_free(snd_pcm_sw_params_t* params) {
        ::snd_pcm_sw_params_free(params);
    }
    int sw_params_current(snd_pcm_t* pcm, snd_pcm_sw_params_t* params) {
        return ::snd_pcm_sw_params_current(pcm, params);
    }
    int sw_params_set_tstamp_mode(snd_pcm_t* pcm, snd_pcm_sw_params_t* params,
                                  snd_pcm_tstamp_t mode) {
        return ::snd_pcm_sw_params_set_tstamp_mode(pcm, params, mode);
    }
    int sw_params_set_tstamp_type(snd_pcm_t* pcm, snd_pcm_sw_params_t* params,
                                  snd_pcm_tstamp_type_t type) {
        return ::snd_pcm_sw_params_set_tstamp_type(pcm, params, type);
    }
    int sw_params_set_avail_min(snd_pcm_t* pcm, snd_pcm_sw_params_t* params,
                                snd_pcm_uframes_t frames) {
        return ::snd_pcm_sw_params_set_avail_min(pcm, params, frames);
    }
    int sw_params(snd_pcm_t* pcm, snd_pcm_sw_params_t* params) {
        return ::snd_pcm_sw_params(pcm, params);
    }
    int pcm_prepare(snd_pcm_t* pcm) { return ::snd_pcm_prepare(pcm); }
    int pcm_start(snd_pcm_t* pcm) { return ::snd_pcm_start(pcm); }
    int pcm_drop(snd_pcm_t* pcm) { return ::snd_pcm_drop(pcm); }
    int pcm_resume(snd_pcm_t* pcm) { return ::snd_pcm_resume(pcm); }
    snd_pcm_sframes_t pcm_readi(snd_pcm_t* pcm, void* buffer,
                                snd_pcm_uframes_t frames) {
        return ::snd_pcm_readi(pcm, buffer, frames);
    }
    int pcm_htimestamp(snd_pcm_t* pcm, snd_pcm_uframes_t* available,
                       snd_htimestamp_t* timestamp) {
        return ::snd_pcm_htimestamp(pcm, available, timestamp);
    }
    snd_pcm_sframes_t pcm_avail_update(snd_pcm_t* pcm) {
        return ::snd_pcm_avail_update(pcm);
    }
    int pcm_poll_descriptors_count(snd_pcm_t* pcm) {
        return ::snd_pcm_poll_descriptors_count(pcm);
    }
    int pcm_poll_descriptors(snd_pcm_t* pcm, struct pollfd* descriptors,
                             unsigned int count) {
        return ::snd_pcm_poll_descriptors(pcm, descriptors, count);
    }
    int pcm_poll_descriptors_revents(
        snd_pcm_t* pcm, struct pollfd* descriptors, unsigned int count,
        unsigned short* revents) {
        return ::snd_pcm_poll_descriptors_revents(
            pcm, descriptors, count, revents);
    }
    int monotonic_now(struct timespec* value) {
        return ::clock_gettime(CLOCK_MONOTONIC, value);
    }
    const char* error_string(int error) const { return ::snd_strerror(error); }
};

}  // namespace

std::unique_ptr<AlsaApi> create_libasound_api() {
    return std::unique_ptr<AlsaApi>(new LibasoundApi());
}

}  // namespace detail
}  // namespace eavp
