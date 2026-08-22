#include "eavp/platform/linux/alsa_capture.hpp"

#include <limits>
#include <memory>
#include <new>

#include "eavp/base/time.hpp"
#include "platform/linux/alsa_capture_internal.hpp"

namespace eavp {

namespace {

const std::int64_t kMicrosecondsPerSecond = 1000000;

Status allocation_failure() {
    return Status(StatusCode::kResourceExhausted,
                  "failed to allocate ALSA capture frame storage");
}

bool samples_to_us(std::int64_t samples, int sample_rate, std::int64_t* result) {
    if (result == NULL || samples < 0 || sample_rate <= 0) return false;
    const std::int64_t rate = static_cast<std::int64_t>(sample_rate);
    const std::int64_t quotient = samples / rate;
    const std::int64_t remainder = samples % rate;
    if (quotient > std::numeric_limits<std::int64_t>::max() /
                       kMicrosecondsPerSecond) {
        return false;
    }
    const std::int64_t seconds_us = quotient * kMicrosecondsPerSecond;
    if (remainder > std::numeric_limits<std::int64_t>::max() /
                        kMicrosecondsPerSecond) {
        return false;
    }
    const std::int64_t remainder_us = (remainder * kMicrosecondsPerSecond) / rate;
    if (seconds_us > std::numeric_limits<std::int64_t>::max() - remainder_us) {
        return false;
    }
    *result = seconds_us + remainder_us;
    return true;
}

}  // namespace

class AlsaSourceNode::Impl {
public:
    Impl(const AlsaCaptureConfig& config_value, MetricRegistry* metrics_value,
         HealthManager* health_value, std::unique_ptr<detail::AlsaSystem> system_value)
        : config(config_value), metrics(metrics_value), health(health_value),
          system(std::move(system_value)), output("audio_output"), partial_buffer(),
          partial_samples(0), pending(), emitted_samples(0), has_anchor(false),
          anchor_pts_us(0), discontinuity_pending(false), timestamp_fallbacks(0) {}

    ~Impl() noexcept {
        if (system) {
            try {
                system->stop();
            } catch (...) {
            }
        }
    }

    Status allocate_partial() {
        if (partial_buffer) return Status::ok_status();
        const std::size_t samples = static_cast<std::size_t>(config.samples_per_frame());
        if (config.format().bytes_per_pcm_frame() >
            std::numeric_limits<std::size_t>::max() / samples) {
            return Status(StatusCode::kInvalidArgument,
                          "ALSA capture frame size overflows");
        }
        const Result<Buffer> result = Buffer::allocate(
            samples * config.format().bytes_per_pcm_frame());
        if (!result.ok()) return result.status();
        try {
            partial_buffer.reset(new Buffer(result.value()));
        } catch (const std::bad_alloc&) {
            return allocation_failure();
        } catch (...) {
            return Status(StatusCode::kInternal,
                          "failed to retain ALSA capture frame storage");
        }
        return Status::ok_status();
    }

    void discard_capture_data() {
        pending.reset();
        partial_buffer.reset();
        partial_samples = 0;
        emitted_samples = 0;
        has_anchor = false;
        anchor_pts_us = 0;
        discontinuity_pending = false;
    }

    void reset_timeline_after_discontinuity() {
        pending.reset();
        partial_buffer.reset();
        partial_samples = 0;
        emitted_samples = 0;
        has_anchor = false;
        anchor_pts_us = 0;
        discontinuity_pending = true;
    }

    Status frame_pts(std::int64_t* result) const {
        if (!has_anchor || result == NULL) {
            return Status(StatusCode::kInvalidState, "ALSA timeline is not anchored");
        }
        std::int64_t delta_us = 0;
        if (!samples_to_us(emitted_samples, config.format().sample_rate(), &delta_us) ||
            (delta_us > 0 && anchor_pts_us >
                                std::numeric_limits<std::int64_t>::max() - delta_us)) {
            return Status(StatusCode::kCorruptData, "ALSA PTS calculation overflows");
        }
        *result = anchor_pts_us + delta_us;
        return Status::ok_status();
    }

    Status advanced_emitted_samples(std::int64_t* result) const {
        const std::int64_t frame_samples =
            static_cast<std::int64_t>(config.samples_per_frame());
        if (result == NULL || emitted_samples >
                                  std::numeric_limits<std::int64_t>::max() - frame_samples) {
            return Status(StatusCode::kCorruptData,
                          "ALSA emitted sample counter overflows");
        }
        *result = emitted_samples + frame_samples;
        return Status::ok_status();
    }

    AlsaCaptureConfig config;
    MetricRegistry* metrics;
    HealthManager* health;
    std::unique_ptr<detail::AlsaSystem> system;
    OutputPort<AudioFrame> output;
    std::unique_ptr<Buffer> partial_buffer;
    int partial_samples;
    std::shared_ptr<const AudioFrame> pending;
    std::int64_t emitted_samples;
    bool has_anchor;
    std::int64_t anchor_pts_us;
    bool discontinuity_pending;
    std::uint64_t timestamp_fallbacks;
};

Result<AlsaCaptureConfig> AlsaCaptureConfig::create(const std::string& device_name,
                                                     const AudioFormat& format,
                                                     int samples_per_frame,
                                                     int period_size_hint,
                                                     int buffer_periods) {
    if (device_name.empty() || samples_per_frame <= 0 ||
        period_size_hint <= 0 || buffer_periods <= 0) {
        return Result<AlsaCaptureConfig>(Status(
            StatusCode::kInvalidArgument,
            "ALSA capture configuration values must be positive"));
    }
    if (format.memory_domain() != MemoryDomain::kCpu ||
        format.sample_layout() != AudioSampleLayout::kInterleaved) {
        return Result<AlsaCaptureConfig>(Status(
            StatusCode::kCapabilityMismatch,
            "ALSA capture requires interleaved CPU audio"));
    }

    const std::size_t frames = static_cast<std::size_t>(samples_per_frame);
    if (format.bytes_per_pcm_frame() >
        std::numeric_limits<std::size_t>::max() / frames) {
        return Result<AlsaCaptureConfig>(Status(
            StatusCode::kInvalidArgument,
            "ALSA capture frame size overflows"));
    }

    try {
        return Result<AlsaCaptureConfig>(AlsaCaptureConfig(
            device_name, format, samples_per_frame, period_size_hint, buffer_periods));
    } catch (const std::bad_alloc&) {
        return Result<AlsaCaptureConfig>(Status(StatusCode::kResourceExhausted));
    } catch (...) {
        return Result<AlsaCaptureConfig>(Status(StatusCode::kInternal));
    }
}

AlsaSourceNode::AlsaSourceNode(const std::string& id, std::unique_ptr<Impl> impl)
    : MediaNode(id), impl_(std::move(impl)) {}

AlsaSourceNode::~AlsaSourceNode() noexcept {}

Result<std::unique_ptr<AlsaSourceNode> > AlsaSourceNode::create(
    const std::string& id, const AlsaCaptureConfig& config,
    MetricRegistry* metrics, HealthManager* health) {
    if (id.empty() || metrics == NULL || health == NULL) {
        return Result<std::unique_ptr<AlsaSourceNode> >(Status(
            StatusCode::kInvalidArgument,
            "ALSA source node id and observers must be configured"));
    }
    try {
        std::unique_ptr<detail::AlsaSystem> system(
            new detail::AlsaSystem(detail::create_libasound_api()));
        std::unique_ptr<Impl> impl(new Impl(config, metrics, health, std::move(system)));
        return Result<std::unique_ptr<AlsaSourceNode> >(
            std::unique_ptr<AlsaSourceNode>(new AlsaSourceNode(id, std::move(impl))));
    } catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<AlsaSourceNode> >(allocation_failure());
    } catch (...) {
        return Result<std::unique_ptr<AlsaSourceNode> >(Status(
            StatusCode::kInternal, "failed to create ALSA source node"));
    }
}

OutputPort<AudioFrame>& AlsaSourceNode::output() { return impl_->output; }

Status AlsaSourceNode::on_prepare() {
    try {
        const Status system_status = impl_->system->prepare(impl_->config);
        if (!system_status.ok()) return system_status;
        const Status buffer_status = impl_->allocate_partial();
        if (!buffer_status.ok()) {
            impl_->system->stop();
            return buffer_status;
        }
        return Status::ok_status();
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    } catch (...) {
        return Status(StatusCode::kInternal, "failed to prepare ALSA source node");
    }
}

Status AlsaSourceNode::on_start() {
    try {
        impl_->discard_capture_data();
        const Status buffer_status = impl_->allocate_partial();
        if (!buffer_status.ok()) return buffer_status;
        return impl_->system->start();
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    } catch (...) {
        return Status(StatusCode::kInternal, "failed to start ALSA source node");
    }
}

Status AlsaSourceNode::on_stop() {
    impl_->discard_capture_data();
    try {
        return impl_->system->stop();
    } catch (...) {
        return Status(StatusCode::kInternal, "failed to stop ALSA source node");
    }
}

Status AlsaSourceNode::on_reset() {
    impl_->discard_capture_data();
    try {
        return impl_->system->stop();
    } catch (...) {
        return Status(StatusCode::kInternal, "failed to reset ALSA source node");
    }
}

Status AlsaSourceNode::on_tick() {
    try {
        if (impl_->pending) {
            std::int64_t next_emitted_samples = 0;
            const Status count_status =
                impl_->advanced_emitted_samples(&next_emitted_samples);
            if (!count_status.ok()) return count_status;
            const Status output_status = impl_->output.send(impl_->pending);
            if (!output_status.ok()) return output_status;
            impl_->pending.reset();
            impl_->emitted_samples = next_emitted_samples;
            return Status::ok_status();
        }

        const Status allocation_status = impl_->allocate_partial();
        if (!allocation_status.ok()) return allocation_status;
        const int missing = impl_->config.samples_per_frame() - impl_->partial_samples;
        if (missing <= 0) {
            return Status(StatusCode::kInternal,
                          "ALSA capture accumulation state is invalid");
        }
        Result<MappedRegion> mapped =
            impl_->partial_buffer->map_plane(0U, MapMode::kReadWrite);
        if (!mapped.ok()) return mapped.status();
        MappedRegion region = mapped.take_value();
        std::uint8_t* destination = region.mutable_data() +
            static_cast<std::size_t>(impl_->partial_samples) *
                impl_->config.format().bytes_per_pcm_frame();
        Result<detail::AlsaAnchor> anchor_candidate(
            Status(StatusCode::kInvalidState, "ALSA timeline already anchored"));
        if (impl_->system->suspend_recovery_pending()) {
            const Result<detail::AlsaReadResult> recovery =
                impl_->system->read_interleaved(destination, missing);
            if (!recovery.ok()) return recovery.status();
            if (recovery.value().timeline_discontinuity) {
                impl_->reset_timeline_after_discontinuity();
            }
            return Status(StatusCode::kWouldBlock, "ALSA capture is resuming");
        }
        if (!impl_->has_anchor) {
            anchor_candidate = impl_->system->capture_anchor();
            if (!anchor_candidate.ok()) return anchor_candidate.status();
        }
        const Result<detail::AlsaReadResult> read =
            impl_->system->read_interleaved(destination, missing);
        if (!read.ok()) return read.status();
        if (read.value().timeline_discontinuity) {
            impl_->reset_timeline_after_discontinuity();
            return Status(StatusCode::kWouldBlock, "ALSA capture recovered timeline");
        }
        if (read.value().would_block) {
            return Status(StatusCode::kWouldBlock, "ALSA capture would block");
        }
        if (!impl_->has_anchor) {
            impl_->has_anchor = true;
            impl_->anchor_pts_us = anchor_candidate.value().first_unread_pts_us;
            if (anchor_candidate.value().used_fallback) {
                if (impl_->timestamp_fallbacks ==
                    std::numeric_limits<std::uint64_t>::max()) {
                    return Status(StatusCode::kCorruptData,
                                  "ALSA timestamp fallback counter overflows");
                }
                ++impl_->timestamp_fallbacks;
            }
        }
        impl_->partial_samples += read.value().frames_read;
        if (impl_->partial_samples < impl_->config.samples_per_frame()) {
            return Status::ok_status();
        }

        std::int64_t pts_us = 0;
        const Status pts_status = impl_->frame_pts(&pts_us);
        if (!pts_status.ok()) return pts_status;
        const Result<AudioFrame> frame = AudioFrame::create(
            *impl_->partial_buffer, impl_->config.format(),
            impl_->config.samples_per_frame(), pts_us,
            TimeBase::create(1, 1000000).value(), impl_->discontinuity_pending);
        if (!frame.ok()) return frame.status();
        try {
            impl_->pending.reset(new AudioFrame(frame.value()));
        } catch (const std::bad_alloc&) {
            return allocation_failure();
        }
        impl_->partial_buffer.reset();
        impl_->partial_samples = 0;
        impl_->discontinuity_pending = false;
        std::int64_t next_emitted_samples = 0;
        const Status count_status = impl_->advanced_emitted_samples(&next_emitted_samples);
        if (!count_status.ok()) return count_status;
        const Status output_status = impl_->output.send(impl_->pending);
        if (!output_status.ok()) return output_status;
        impl_->pending.reset();
        impl_->emitted_samples = next_emitted_samples;
        return Status::ok_status();
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    } catch (...) {
        return Status(StatusCode::kInternal, "failed to capture ALSA audio");
    }
}

namespace detail {

Result<std::unique_ptr<AlsaSourceNode> > AlsaSourceNodeTestPeer::create(
    const std::string& id, const AlsaCaptureConfig& config,
    MetricRegistry* metrics, HealthManager* health,
    std::unique_ptr<AlsaSystem> system) {
    if (id.empty() || metrics == NULL || health == NULL || !system) {
        return Result<std::unique_ptr<AlsaSourceNode> >(Status(
            StatusCode::kInvalidArgument,
            "ALSA source node id, observers and system must be configured"));
    }
    try {
        std::unique_ptr<AlsaSourceNode::Impl> impl(
            new AlsaSourceNode::Impl(config, metrics, health, std::move(system)));
        return Result<std::unique_ptr<AlsaSourceNode> >(
            std::unique_ptr<AlsaSourceNode>(new AlsaSourceNode(id, std::move(impl))));
    } catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<AlsaSourceNode> >(allocation_failure());
    } catch (...) {
        return Result<std::unique_ptr<AlsaSourceNode> >(Status(
            StatusCode::kInternal, "failed to create ALSA source test node"));
    }
}

}  // namespace detail

}  // namespace eavp
