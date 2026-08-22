#include "eavp/platform/linux/alsa_capture.hpp"

#include <limits>
#include <memory>
#include <new>
#include <string>

#include "eavp/base/time.hpp"
#include "eavp/management/health.hpp"
#include "eavp/management/metrics.hpp"
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

template <typename ObserverCall>
Status invoke_observer(const ObserverCall& call) {
    try {
        return call();
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::kResourceExhausted,
                      "ALSA observer exhausted resources");
    } catch (...) {
        return Status(StatusCode::kInternal, "ALSA observer failed");
    }
}

class RegistryAlsaObserver : public detail::AlsaObserver {
public:
    RegistryAlsaObserver(const std::string& node_id, MetricRegistry* metrics,
                         HealthManager* health, int samples_per_frame)
        : metrics_(metrics), health_(health), metric_prefix_("alsa_capture." + node_id + "."),
          health_component_("alsa_capture/" + node_id),
          samples_per_frame_(samples_per_frame), timestamp_fallbacks_(0U),
          xruns_(0U), suspends_(0U), recoveries_(0U) {}

    Status on_negotiated(int period_frames, int buffer_frames) override {
        timestamp_fallbacks_ = 0U;
        xruns_ = 0U;
        suspends_ = 0U;
        recoveries_ = 0U;
        Status result = metrics_->set_gauge(metric_prefix_ + "actual_period_frames",
                                            static_cast<double>(period_frames));
        record(metrics_->set_gauge(metric_prefix_ + "actual_buffer_frames",
                                   static_cast<double>(buffer_frames)), &result);
        record(metrics_->set_gauge(metric_prefix_ + "partial_samples", 0.0), &result);
        record(health_->report(health_component_, HealthStatus::kOk,
                               "ALSA capture is running"), &result);
        return result;
    }

    Status on_partial(int partial_samples) override {
        Status result = metrics_->set_gauge(metric_prefix_ + "partial_samples",
                                            static_cast<double>(partial_samples));
        if (partial_samples > 0 && partial_samples < samples_per_frame_) {
            record(metrics_->increment_counter(metric_prefix_ + "short_reads"), &result);
        }
        return result;
    }

    Status on_would_block() override {
        return metrics_->increment_counter(metric_prefix_ + "would_block");
    }

    Status on_frame(const AudioFrame& frame) override {
        Status result = metrics_->increment_counter(metric_prefix_ + "frames");
        record(metrics_->increment_counter(
                   metric_prefix_ + "samples",
                   static_cast<std::uint64_t>(frame.samples_per_channel())), &result);
        record(metrics_->increment_counter(
                   metric_prefix_ + "bytes",
                   static_cast<std::uint64_t>(frame.buffer().plane_layout(0U).value().size)),
               &result);
        record(metrics_->set_gauge(metric_prefix_ + "last_pts_us",
                                   static_cast<double>(frame.pts())), &result);
        return result;
    }

    Status on_timestamp_fallback() override {
        ++timestamp_fallbacks_;
        Status result = metrics_->increment_counter(metric_prefix_ + "timestamp_fallbacks");
        record(report_degraded("timestamp fallback"), &result);
        return result;
    }

    Status on_recovery(bool xrun) override {
        if (xrun) {
            ++xruns_;
        } else {
            ++suspends_;
        }
        ++recoveries_;
        Status result = metrics_->increment_counter(
            metric_prefix_ + (xrun ? "xruns" : "suspends"));
        record(metrics_->increment_counter(metric_prefix_ + "recoveries"), &result);
        record(metrics_->increment_counter(metric_prefix_ + "discontinuities"), &result);
        record(report_degraded(xrun ? "XRUN recovery" : "suspend recovery"), &result);
        return result;
    }

    Status on_fatal(const Status& failure) override {
        const std::string message = failure.message().empty()
            ? "ALSA capture failed" : failure.message();
        return health_->report(health_component_, HealthStatus::kError, message);
    }

private:
    static void record(const Status& candidate, Status* result) {
        if (result->ok() && !candidate.ok()) *result = candidate;
    }

    Status report_degraded(const std::string& reason) {
        return health_->report(
            health_component_, HealthStatus::kDegraded,
            reason + "; timestamp_fallbacks=" + std::to_string(timestamp_fallbacks_) +
            ", xruns=" + std::to_string(xruns_) +
            ", suspends=" + std::to_string(suspends_) +
            ", recoveries=" + std::to_string(recoveries_));
    }

    MetricRegistry* metrics_;
    HealthManager* health_;
    std::string metric_prefix_;
    std::string health_component_;
    int samples_per_frame_;
    std::uint64_t timestamp_fallbacks_;
    std::uint64_t xruns_;
    std::uint64_t suspends_;
    std::uint64_t recoveries_;
};

}  // namespace

class AlsaSourceNode::Impl {
public:
    Impl(const std::string& id_for_observer, const AlsaCaptureConfig& config_value,
         MetricRegistry* metrics_value,
         HealthManager* health_value, std::unique_ptr<detail::AlsaSystem> system_value,
         detail::AlsaObserver* observer_value)
        : config(config_value), metrics(metrics_value), health(health_value),
          system(std::move(system_value)), output("audio_output"), partial_buffer(),
          partial_samples(0), pending(), emitted_samples(0), has_anchor(false),
          anchor_pts_us(0), discontinuity_pending(false), timestamp_fallbacks(0),
          owned_observer(), observer(observer_value) {
        if (observer == NULL) {
            owned_observer.reset(new RegistryAlsaObserver(
                id_for_observer, metrics, health, config.samples_per_frame()));
            observer = owned_observer.get();
        }
    }

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

    Status report_media_failure(const Status& media_status) {
        if (media_status.code() != StatusCode::kWouldBlock) {
            invoke_observer([this, &media_status]() {
                return observer->on_fatal(media_status);
            });
        }
        return media_status;
    }

    Status report_would_block(const Status& media_status) {
        const Status observer_status = invoke_observer([this]() {
            return observer->on_would_block();
        });
        return observer_status.ok() ? media_status : observer_status;
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
    std::unique_ptr<detail::AlsaObserver> owned_observer;
    detail::AlsaObserver* observer;
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
        std::unique_ptr<Impl> impl(
            new Impl(id, config, metrics, health, std::move(system), NULL));
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
        const Status start_status = impl_->system->start();
        if (!start_status.ok()) return start_status;
        const detail::AlsaNegotiatedParameters& negotiated = impl_->system->negotiated();
        if (negotiated.period_frames >
                static_cast<snd_pcm_uframes_t>(std::numeric_limits<int>::max()) ||
            negotiated.buffer_frames >
                static_cast<snd_pcm_uframes_t>(std::numeric_limits<int>::max())) {
            return impl_->report_media_failure(Status(
                StatusCode::kCorruptData, "ALSA negotiated frame count is too large"));
        }
        const Status observer_status = invoke_observer([this, &negotiated]() {
            return impl_->observer->on_negotiated(
                static_cast<int>(negotiated.period_frames),
                static_cast<int>(negotiated.buffer_frames));
        });
        return observer_status;
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
        const Status stop_status = impl_->system->stop();
        if (!stop_status.ok()) return stop_status;
        return invoke_observer([this]() {
            return impl_->observer->on_negotiated(0, 0);
        });
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
            if (!count_status.ok()) return impl_->report_media_failure(count_status);
            const Status output_status = impl_->output.send(impl_->pending);
            if (!output_status.ok()) {
                if (output_status.code() == StatusCode::kWouldBlock) {
                    return impl_->report_would_block(output_status);
                }
                return impl_->report_media_failure(output_status);
            }
            const std::shared_ptr<const AudioFrame> delivered = impl_->pending;
            impl_->pending.reset();
            impl_->emitted_samples = next_emitted_samples;
            return invoke_observer([this, &delivered]() {
                return impl_->observer->on_frame(*delivered);
            });
        }

        const Status allocation_status = impl_->allocate_partial();
        if (!allocation_status.ok()) return impl_->report_media_failure(allocation_status);
        const int missing = impl_->config.samples_per_frame() - impl_->partial_samples;
        if (missing <= 0) {
            return impl_->report_media_failure(Status(
                StatusCode::kInternal, "ALSA capture accumulation state is invalid"));
        }
        Result<MappedRegion> mapped =
            impl_->partial_buffer->map_plane(0U, MapMode::kReadWrite);
        if (!mapped.ok()) return impl_->report_media_failure(mapped.status());
        MappedRegion region = mapped.take_value();
        std::uint8_t* destination = region.mutable_data() +
            static_cast<std::size_t>(impl_->partial_samples) *
                impl_->config.format().bytes_per_pcm_frame();
        Result<detail::AlsaAnchor> anchor_candidate(
            Status(StatusCode::kInvalidState, "ALSA timeline already anchored"));
        if (impl_->system->suspend_recovery_pending()) {
            const Result<detail::AlsaReadResult> recovery =
                impl_->system->read_interleaved(destination, missing);
            if (!recovery.ok()) return impl_->report_media_failure(recovery.status());
            if (recovery.value().timeline_discontinuity) {
                impl_->reset_timeline_after_discontinuity();
                const Status observer_status = invoke_observer([this]() {
                    return impl_->observer->on_partial(0);
                });
                if (!observer_status.ok()) return observer_status;
            }
            if (!impl_->system->suspend_recovery_pending()) {
                const Status observer_status = invoke_observer([this]() {
                    return impl_->observer->on_recovery(false);
                });
                if (!observer_status.ok()) return observer_status;
            }
            return impl_->report_would_block(
                Status(StatusCode::kWouldBlock, "ALSA capture is resuming"));
        }
        if (!impl_->has_anchor) {
            anchor_candidate = impl_->system->capture_anchor();
            if (!anchor_candidate.ok()) {
                return impl_->report_media_failure(anchor_candidate.status());
            }
        }
        const Result<detail::AlsaReadResult> read =
            impl_->system->read_interleaved(destination, missing);
        if (!read.ok()) return impl_->report_media_failure(read.status());
        if (read.value().timeline_discontinuity) {
            const bool xrun = !impl_->system->suspend_recovery_pending();
            impl_->reset_timeline_after_discontinuity();
            const Status partial_status = invoke_observer([this]() {
                return impl_->observer->on_partial(0);
            });
            if (!partial_status.ok()) return partial_status;
            if (xrun) {
                const Status observer_status = invoke_observer([this]() {
                    return impl_->observer->on_recovery(true);
                });
                if (!observer_status.ok()) return observer_status;
            }
            return impl_->report_would_block(Status(
                StatusCode::kWouldBlock, "ALSA capture recovered timeline"));
        }
        if (read.value().would_block) {
            return impl_->report_would_block(
                Status(StatusCode::kWouldBlock, "ALSA capture would block"));
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
                const Status observer_status = invoke_observer([this]() {
                    return impl_->observer->on_timestamp_fallback();
                });
                if (!observer_status.ok()) return observer_status;
            }
        }
        impl_->partial_samples += read.value().frames_read;
        if (impl_->partial_samples < impl_->config.samples_per_frame()) {
            return invoke_observer([this]() {
                return impl_->observer->on_partial(impl_->partial_samples);
            });
        }

        std::int64_t pts_us = 0;
        const Status pts_status = impl_->frame_pts(&pts_us);
        if (!pts_status.ok()) return impl_->report_media_failure(pts_status);
        const Result<AudioFrame> frame = AudioFrame::create(
            *impl_->partial_buffer, impl_->config.format(),
            impl_->config.samples_per_frame(), pts_us,
            TimeBase::create(1, 1000000).value(), impl_->discontinuity_pending);
        if (!frame.ok()) return impl_->report_media_failure(frame.status());
        try {
            impl_->pending.reset(new AudioFrame(frame.value()));
        } catch (const std::bad_alloc&) {
            return impl_->report_media_failure(allocation_failure());
        }
        impl_->partial_buffer.reset();
        impl_->partial_samples = 0;
        impl_->discontinuity_pending = false;
        const Status partial_status = invoke_observer([this]() {
            return impl_->observer->on_partial(0);
        });
        if (!partial_status.ok()) return partial_status;
        std::int64_t next_emitted_samples = 0;
        const Status count_status = impl_->advanced_emitted_samples(&next_emitted_samples);
        if (!count_status.ok()) return impl_->report_media_failure(count_status);
        const Status output_status = impl_->output.send(impl_->pending);
        if (!output_status.ok()) {
            if (output_status.code() == StatusCode::kWouldBlock) {
                return impl_->report_would_block(output_status);
            }
            return impl_->report_media_failure(output_status);
        }
        const std::shared_ptr<const AudioFrame> delivered = impl_->pending;
        impl_->pending.reset();
        impl_->emitted_samples = next_emitted_samples;
        return invoke_observer([this, &delivered]() {
            return impl_->observer->on_frame(*delivered);
        });
    } catch (const std::bad_alloc&) {
        return impl_->report_media_failure(allocation_failure());
    } catch (...) {
        return impl_->report_media_failure(Status(
            StatusCode::kInternal, "failed to capture ALSA audio"));
    }
}

namespace detail {

Result<std::unique_ptr<AlsaSourceNode> > AlsaSourceNodeTestPeer::create(
    const std::string& id, const AlsaCaptureConfig& config,
    MetricRegistry* metrics, HealthManager* health,
    std::unique_ptr<AlsaSystem> system, AlsaObserver* observer) {
    if (id.empty() || metrics == NULL || health == NULL || !system) {
        return Result<std::unique_ptr<AlsaSourceNode> >(Status(
            StatusCode::kInvalidArgument,
            "ALSA source node id, observers and system must be configured"));
    }
    try {
        std::unique_ptr<AlsaSourceNode::Impl> impl(
            new AlsaSourceNode::Impl(id, config, metrics, health, std::move(system),
                                     observer));
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
