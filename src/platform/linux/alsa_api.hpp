#ifndef EAVP_PLATFORM_LINUX_ALSA_API_HPP_
#define EAVP_PLATFORM_LINUX_ALSA_API_HPP_

#include <ctime>

#include <alsa/asoundlib.h>

namespace eavp {
namespace detail {

class AlsaApi {
public:
    virtual ~AlsaApi() {}

    virtual int pcm_open(snd_pcm_t** pcm, const char* name,
                         snd_pcm_stream_t stream, int mode) = 0;
    virtual int pcm_close(snd_pcm_t* pcm) = 0;
    virtual int hw_params_malloc(snd_pcm_hw_params_t** params) = 0;
    virtual void hw_params_free(snd_pcm_hw_params_t* params) = 0;
    virtual int hw_params_any(snd_pcm_t* pcm, snd_pcm_hw_params_t* params) = 0;
    virtual int hw_params_set_access(snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
                                     snd_pcm_access_t access) = 0;
    virtual int hw_params_set_format(snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
                                     snd_pcm_format_t format) = 0;
    virtual int hw_params_set_channels(snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
                                       unsigned int channels) = 0;
    virtual int hw_params_set_rate(snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
                                   unsigned int rate, int direction) = 0;
    virtual int hw_params_set_period_size_near(
        snd_pcm_t* pcm, snd_pcm_hw_params_t* params, snd_pcm_uframes_t* frames,
        int* direction) = 0;
    virtual int hw_params_set_buffer_size_near(
        snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
        snd_pcm_uframes_t* frames) = 0;
    virtual int hw_params(snd_pcm_t* pcm, snd_pcm_hw_params_t* params) = 0;
    virtual int hw_params_get_access(const snd_pcm_hw_params_t* params,
                                     snd_pcm_access_t* access) = 0;
    virtual int hw_params_get_format(const snd_pcm_hw_params_t* params,
                                     snd_pcm_format_t* format) = 0;
    virtual int hw_params_get_channels(const snd_pcm_hw_params_t* params,
                                       unsigned int* channels) = 0;
    virtual int hw_params_get_rate(const snd_pcm_hw_params_t* params,
                                   unsigned int* rate, int* direction) = 0;
    virtual int hw_params_get_period_size(const snd_pcm_hw_params_t* params,
                                          snd_pcm_uframes_t* frames,
                                          int* direction) = 0;
    virtual int hw_params_get_buffer_size(const snd_pcm_hw_params_t* params,
                                          snd_pcm_uframes_t* frames) = 0;
    virtual int sw_params_malloc(snd_pcm_sw_params_t** params) = 0;
    virtual void sw_params_free(snd_pcm_sw_params_t* params) = 0;
    virtual int sw_params_current(snd_pcm_t* pcm, snd_pcm_sw_params_t* params) = 0;
    virtual int sw_params_set_tstamp_mode(snd_pcm_t* pcm,
                                          snd_pcm_sw_params_t* params,
                                          snd_pcm_tstamp_t mode) = 0;
    virtual int sw_params_set_tstamp_type(snd_pcm_t* pcm,
                                          snd_pcm_sw_params_t* params,
                                          snd_pcm_tstamp_type_t type) = 0;
    virtual int sw_params_set_avail_min(snd_pcm_t* pcm,
                                        snd_pcm_sw_params_t* params,
                                        snd_pcm_uframes_t frames) = 0;
    virtual int sw_params(snd_pcm_t* pcm, snd_pcm_sw_params_t* params) = 0;
    virtual int pcm_prepare(snd_pcm_t* pcm) = 0;
    virtual int pcm_start(snd_pcm_t* pcm) = 0;
    virtual int pcm_drop(snd_pcm_t* pcm) = 0;
    virtual int pcm_resume(snd_pcm_t* pcm) = 0;
    virtual snd_pcm_sframes_t pcm_readi(snd_pcm_t* pcm, void* buffer,
                                        snd_pcm_uframes_t frames) = 0;
    virtual int pcm_htimestamp(snd_pcm_t* pcm, snd_pcm_uframes_t* available,
                               snd_htimestamp_t* timestamp) = 0;
    virtual snd_pcm_sframes_t pcm_avail_update(snd_pcm_t* pcm) = 0;
    virtual int pcm_poll_descriptors_count(snd_pcm_t* pcm) = 0;
    virtual int pcm_poll_descriptors(snd_pcm_t* pcm,
                                     struct pollfd* descriptors,
                                     unsigned int count) = 0;
    virtual int pcm_poll_descriptors_revents(
        snd_pcm_t* pcm, struct pollfd* descriptors, unsigned int count,
        unsigned short* revents) = 0;
    virtual int monotonic_now(struct timespec* value) = 0;
    virtual const char* error_string(int error) const = 0;
};

}  // namespace detail
}  // namespace eavp

#endif  // EAVP_PLATFORM_LINUX_ALSA_API_HPP_
