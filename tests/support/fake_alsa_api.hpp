#ifndef EAVP_TESTS_SUPPORT_FAKE_ALSA_API_HPP_
#define EAVP_TESTS_SUPPORT_FAKE_ALSA_API_HPP_

#include <cerrno>
#include <ctime>
#include <deque>
#include <poll.h>
#include <vector>

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
          sw_params_alloc_count(0), sw_params_free_count(0), pcm_prepare_count(0),
          pcm_start_count(0), pcm_drop_count(0), pcm_resume_count(0),
          htimestamp_count(0), avail_update_count(0), monotonic_now_count(0),
          poll_descriptors_count_calls(0), poll_descriptors_calls(0),
          poll_revents_calls(0),
          htimestamp_result(0), htimestamp_available(0U),
          htimestamp_value(), avail_update_result(0), monotonic_now_result(0),
          monotonic_now_value(), pcm_prepare_results(), pcm_start_results(),
          pcm_resume_results(), pcm_read_results(), poll_descriptor_values(),
          poll_descriptor_arrays(),
          poll_descriptor_count_results(), poll_descriptor_results(),
          poll_revents_results(), poll_revents_values(),
          poll_revents_value(0U), poll_descriptor_spaces(),
          poll_revents_counts(), last_poll_revents_descriptors() {}

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
    int pcm_prepare(snd_pcm_t*) {
        ++pcm_prepare_count;
        return scripted_result(pcm_prepare_results, kPcmPrepare);
    }
    int pcm_start(snd_pcm_t*) {
        ++pcm_start_count;
        return scripted_result(pcm_start_results, kPcmStart);
    }
    int pcm_drop(snd_pcm_t*) {
        ++pcm_drop_count;
        return result(kPcmDrop);
    }
    int pcm_resume(snd_pcm_t*) {
        ++pcm_resume_count;
        return scripted_result(pcm_resume_results, 0);
    }
    snd_pcm_sframes_t pcm_readi(snd_pcm_t*, void*, snd_pcm_uframes_t) {
        if (pcm_read_results.empty()) return 0;
        const snd_pcm_sframes_t value = pcm_read_results.front();
        pcm_read_results.pop_front();
        return value;
    }
    int pcm_htimestamp(snd_pcm_t*, snd_pcm_uframes_t* available,
                       snd_htimestamp_t* timestamp) {
        ++htimestamp_count;
        *available = htimestamp_available;
        *timestamp = htimestamp_value;
        return htimestamp_result;
    }
    snd_pcm_sframes_t pcm_avail_update(snd_pcm_t*) {
        ++avail_update_count;
        return avail_update_result;
    }
    int pcm_poll_descriptors_count(snd_pcm_t*) override {
        ++poll_descriptors_count_calls;
        return scripted_result(
            poll_descriptor_count_results,
            static_cast<int>(poll_descriptor_values.size()));
    }
    int pcm_poll_descriptors(snd_pcm_t*, struct pollfd* descriptors,
                             unsigned int count) override {
        ++poll_descriptors_calls;
        poll_descriptor_spaces.push_back(count);
        std::vector<struct pollfd> scripted_values;
        const std::vector<struct pollfd>* values = &poll_descriptor_values;
        if (!poll_descriptor_arrays.empty()) {
            scripted_values = poll_descriptor_arrays.front();
            poll_descriptor_arrays.pop_front();
            values = &scripted_values;
        }
        const int scripted = scripted_result(
            poll_descriptor_results,
            static_cast<int>(values->size()));
        if (scripted < 0) return scripted;
        const unsigned int copied =
            count < values->size()
                ? count
                : static_cast<unsigned int>(values->size());
        for (unsigned int index = 0U; index < copied; ++index) {
            descriptors[index] = (*values)[index];
        }
        return scripted;
    }
    int pcm_poll_descriptors_revents(
        snd_pcm_t*, struct pollfd* descriptors, unsigned int count,
        unsigned short* revents) override {
        ++poll_revents_calls;
        poll_revents_counts.push_back(count);
        last_poll_revents_descriptors.assign(descriptors, descriptors + count);
        const int scripted = scripted_result(poll_revents_results, 0);
        if (scripted < 0) return scripted;
        if (poll_revents_values.empty()) {
            *revents = poll_revents_value;
        } else {
            *revents = poll_revents_values.front();
            poll_revents_values.pop_front();
        }
        return scripted;
    }
    int monotonic_now(struct timespec* value) {
        ++monotonic_now_count;
        *value = monotonic_now_value;
        return monotonic_now_result;
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
    int pcm_prepare_count;
    int pcm_start_count;
    int pcm_drop_count;
    int pcm_resume_count;
    int htimestamp_count;
    int avail_update_count;
    int monotonic_now_count;
    int poll_descriptors_count_calls;
    int poll_descriptors_calls;
    int poll_revents_calls;
    int htimestamp_result;
    snd_pcm_uframes_t htimestamp_available;
    snd_htimestamp_t htimestamp_value;
    snd_pcm_sframes_t avail_update_result;
    int monotonic_now_result;
    struct timespec monotonic_now_value;
    std::deque<int> pcm_prepare_results;
    std::deque<int> pcm_start_results;
    std::deque<int> pcm_resume_results;
    std::deque<snd_pcm_sframes_t> pcm_read_results;
    std::vector<struct pollfd> poll_descriptor_values;
    std::deque<std::vector<struct pollfd> > poll_descriptor_arrays;
    std::deque<int> poll_descriptor_count_results;
    std::deque<int> poll_descriptor_results;
    std::deque<int> poll_revents_results;
    std::deque<unsigned short> poll_revents_values;
    unsigned short poll_revents_value;
    std::vector<unsigned int> poll_descriptor_spaces;
    std::vector<unsigned int> poll_revents_counts;
    std::vector<struct pollfd> last_poll_revents_descriptors;

private:
    bool fails(Step step) const { return fail_step == static_cast<int>(step); }
    int result(Step step) const { return fails(step) ? fail_code : 0; }
    int scripted_result(std::deque<int>& values, Step step) {
        if (values.empty()) return result(step);
        const int value = values.front();
        values.pop_front();
        return value;
    }
    int scripted_result(std::deque<int>& values, int fallback) {
        if (values.empty()) return fallback;
        const int value = values.front();
        values.pop_front();
        return value;
    }
};

}  // namespace eavp_test

#endif  // EAVP_TESTS_SUPPORT_FAKE_ALSA_API_HPP_
