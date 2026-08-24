#ifndef EAVP_TESTS_SUPPORT_AUDIO_TEST_UTILS_HPP_
#define EAVP_TESTS_SUPPORT_AUDIO_TEST_UTILS_HPP_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "eavp/platform/linux/alsa_capture.hpp"
#include "platform/linux/alsa_system.hpp"
#include "support/fake_alsa_api.hpp"

namespace eavp_test {

inline eavp::AlsaCaptureConfig make_alsa_config(
    eavp::SampleFormat sample_format =
        eavp::SampleFormat::kSigned16LittleEndian) {
    const eavp::Result<eavp::AudioFormat> format = eavp::AudioFormat::create(
        sample_format, 48000, eavp::AudioChannelLayout::kStereo,
        eavp::AudioSampleLayout::kInterleaved, eavp::MemoryDomain::kCpu);
    const eavp::Result<eavp::AlsaCaptureConfig> config =
        eavp::AlsaCaptureConfig::create("hw:Fake,0", format.value(), 480,
                                        480, 4);
    return config.value();
}

inline std::size_t pcm_bytes(std::size_t frames, std::size_t channels,
                             std::size_t bytes_per_sample) {
    return frames * channels * bytes_per_sample;
}

class ScriptedAlsa {
public:
    struct Event {
        explicit Event(snd_pcm_sframes_t result_value) : result(result_value) {}
        snd_pcm_sframes_t result;
    };

    struct State {
        State() : read_calls(0), prepared_after_start(0), starts_after_start(0),
                  open_handles(0), started(false), next_byte(0U), read_events(), resume_events(),
                  has_htimestamp(false), htimestamp_us(0), htimestamp_available(0U),
                  has_initial_anchor(false), initial_anchor_us(0),
                  has_recovery_anchor(false), recovery_anchor_us(0),
                  force_timestamp_fallback(false),
                  scratch(8192U, 0U) {}

        int read_calls;
        int prepared_after_start;
        int starts_after_start;
        int open_handles;
        bool started;
        std::uint8_t next_byte;
        std::deque<Event> read_events;
        std::deque<int> resume_events;
        bool has_htimestamp;
        std::int64_t htimestamp_us;
        snd_pcm_uframes_t htimestamp_available;
        bool has_initial_anchor;
        std::int64_t initial_anchor_us;
        bool has_recovery_anchor;
        std::int64_t recovery_anchor_us;
        bool force_timestamp_fallback;
        std::vector<std::uint8_t> scratch;
    };

    class EventAppender {
    public:
        EventAppender(const std::shared_ptr<State>& state, bool resume)
            : state_(state), resume_(resume) {}
        void push_back(snd_pcm_sframes_t result) {
            if (resume_) {
                state_->resume_events.push_back(result);
            } else {
                state_->read_events.push_back(Event(result));
            }
        }

    private:
        std::shared_ptr<State> state_;
        bool resume_;
    };

    ScriptedAlsa()
        : state_(new State()), read_frames(state_, false), read_errors(state_, false),
          resume_results(state_, true) {}

    std::unique_ptr<eavp::detail::AlsaSystem> take_system();
    eavp::detail::AlsaSystem take_started_system();

    void append_complete_frames(int count) {
        for (int index = 0; index < count; ++index) read_frames.push_back(480);
    }
    void append_patterned_frames(int count, int first, int second) {
        for (int index = 0; index < count; ++index) {
            read_frames.push_back(first);
            read_frames.push_back(second);
        }
    }
    void append_error(int error) { read_errors.push_back(error); }
    void set_htimestamp(std::int64_t timestamp_us, snd_pcm_uframes_t available) {
        state_->has_htimestamp = true;
        state_->htimestamp_us = timestamp_us;
        state_->htimestamp_available = available;
    }
    void set_initial_anchor(std::int64_t anchor_us) {
        state_->has_initial_anchor = true;
        state_->initial_anchor_us = anchor_us;
    }
    void set_recovery_anchor(std::int64_t anchor_us) {
        state_->has_recovery_anchor = true;
        state_->recovery_anchor_us = anchor_us;
    }
    void force_timestamp_fallback() { state_->force_timestamp_fallback = true; }
    std::uint8_t* bytes() { return state_->scratch.data(); }
    int observed_read_calls() const { return state_->read_calls; }
    int prepare_after_xrun_calls() const { return state_->prepared_after_start; }
    int start_after_xrun_calls() const { return state_->starts_after_start; }
    int open_handles() const { return state_->open_handles; }

private:
    std::shared_ptr<State> state_;

public:
    EventAppender read_frames;
    EventAppender read_errors;
    EventAppender resume_results;
};

class ScriptedAlsaApi : public FakeAlsaApi {
public:
    explicit ScriptedAlsaApi(const std::shared_ptr<ScriptedAlsa::State>& state)
        : state_(state) {}

    int pcm_open(snd_pcm_t** pcm, const char* name, snd_pcm_stream_t stream,
                 int mode) override {
        const int result = FakeAlsaApi::pcm_open(pcm, name, stream, mode);
        if (result >= 0) ++state_->open_handles;
        return result;
    }
    int pcm_close(snd_pcm_t* pcm) override {
        const int result = FakeAlsaApi::pcm_close(pcm);
        if (result >= 0) --state_->open_handles;
        return result;
    }
    int pcm_prepare(snd_pcm_t* pcm) override {
        if (state_->started) ++state_->prepared_after_start;
        return FakeAlsaApi::pcm_prepare(pcm);
    }
    int pcm_start(snd_pcm_t* pcm) override {
        if (state_->started) ++state_->starts_after_start;
        const int result = FakeAlsaApi::pcm_start(pcm);
        if (result >= 0) state_->started = true;
        return result;
    }
    snd_pcm_sframes_t pcm_readi(snd_pcm_t*, void* destination,
                                snd_pcm_uframes_t requested) override {
        ++state_->read_calls;
        if (state_->read_events.empty()) return 0;
        const snd_pcm_sframes_t result = state_->read_events.front().result;
        state_->read_events.pop_front();
        if (result > 0 && destination != NULL) {
            const std::size_t frames = std::min(
                static_cast<std::size_t>(result), static_cast<std::size_t>(requested));
            const std::size_t bytes = std::min(
                frames * 4U, state_->scratch.size());
            std::uint8_t* output = static_cast<std::uint8_t*>(destination);
            for (std::size_t index = 0U; index < bytes; ++index) {
                output[index] = state_->next_byte++;
            }
        }
        return result;
    }
    int pcm_resume(snd_pcm_t* pcm) override {
        if (state_->resume_events.empty()) return FakeAlsaApi::pcm_resume(pcm);
        const int result = state_->resume_events.front();
        state_->resume_events.pop_front();
        return result;
    }
    int pcm_htimestamp(snd_pcm_t* pcm, snd_pcm_uframes_t* available,
                       snd_htimestamp_t* timestamp) override {
        if (state_->force_timestamp_fallback) return -EIO;
        if (!state_->has_htimestamp && !state_->has_initial_anchor) {
            return FakeAlsaApi::pcm_htimestamp(pcm, available, timestamp);
        }
        ++htimestamp_count;
        const std::int64_t timestamp_us =
            state_->has_htimestamp ? state_->htimestamp_us :
            (state_->has_recovery_anchor && state_->prepared_after_start > 0
                 ? state_->recovery_anchor_us : state_->initial_anchor_us);
        *available = state_->has_htimestamp ? state_->htimestamp_available : 0U;
        timestamp->tv_sec = static_cast<time_t>(timestamp_us / 1000000);
        timestamp->tv_nsec = static_cast<long>(
            (timestamp_us % 1000000) * 1000);
        return 0;
    }

private:
    std::shared_ptr<ScriptedAlsa::State> state_;
};

inline std::unique_ptr<eavp::detail::AlsaSystem> ScriptedAlsa::take_system() {
    return std::unique_ptr<eavp::detail::AlsaSystem>(
        new eavp::detail::AlsaSystem(
            std::unique_ptr<eavp::detail::AlsaApi>(new ScriptedAlsaApi(state_))));
}

inline eavp::detail::AlsaSystem ScriptedAlsa::take_started_system() {
    std::unique_ptr<eavp::detail::AlsaSystem> system = take_system();
    system->prepare(make_alsa_config());
    system->start();
    return std::move(*system);
}

}  // namespace eavp_test

#endif  // EAVP_TESTS_SUPPORT_AUDIO_TEST_UTILS_HPP_
