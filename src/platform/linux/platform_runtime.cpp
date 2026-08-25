#include "eavp/platform/linux/platform_runtime.hpp"

#include <new>

namespace eavp {

class LinuxPlatformRuntime::Impl {
public:
    Impl(const LinuxPlatformRuntimeConfig& config, MetricRegistry* metrics)
        : config_(config), metrics_(metrics), state_(PlatformRuntimeState::kCreated),
          last_failure_(StatusCode::kInvalidState) {}

    LinuxPlatformRuntimeConfig config_;
    MetricRegistry* metrics_;
    PlatformRuntimeState state_;
    Status last_failure_;
};

Result<LinuxPlatformRuntimeConfig> LinuxPlatformRuntimeConfig::create(
    int reactor_count, int stop_timeout_ms) {
    if (reactor_count <= 0 || stop_timeout_ms <= 0) {
        return Result<LinuxPlatformRuntimeConfig>(Status(StatusCode::kInvalidArgument));
    }
    if (reactor_count != 1) {
        return Result<LinuxPlatformRuntimeConfig>(Status(StatusCode::kUnsupported));
    }

    try {
        return Result<LinuxPlatformRuntimeConfig>(
            LinuxPlatformRuntimeConfig(reactor_count, stop_timeout_ms));
    } catch (const std::bad_alloc&) {
        return Result<LinuxPlatformRuntimeConfig>(Status(StatusCode::kResourceExhausted));
    } catch (...) {
        return Result<LinuxPlatformRuntimeConfig>(Status(StatusCode::kInternal));
    }
}

Result<std::unique_ptr<LinuxPlatformRuntime> > LinuxPlatformRuntime::create(
    const LinuxPlatformRuntimeConfig& config, MetricRegistry* metrics) {
    try {
        std::unique_ptr<LinuxPlatformRuntime> runtime(
            new LinuxPlatformRuntime(config, metrics));
        return Result<std::unique_ptr<LinuxPlatformRuntime> >(std::move(runtime));
    } catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<LinuxPlatformRuntime> >(
            Status(StatusCode::kResourceExhausted));
    } catch (...) {
        return Result<std::unique_ptr<LinuxPlatformRuntime> >(Status(StatusCode::kInternal));
    }
}

LinuxPlatformRuntime::~LinuxPlatformRuntime() noexcept {}

Status LinuxPlatformRuntime::register_pipeline(
    MediaPipeline* pipeline, const std::vector<LinuxWaitSource*>& wait_sources) {
    (void)pipeline;
    (void)wait_sources;
    return Status(StatusCode::kInvalidState);
}

Status LinuxPlatformRuntime::start() {
    return Status(StatusCode::kInvalidState);
}

Status LinuxPlatformRuntime::stop() {
    return Status(StatusCode::kInvalidState);
}

PlatformRuntimeState LinuxPlatformRuntime::state() const {
    return impl_->state_;
}

Status LinuxPlatformRuntime::last_failure() const {
    return impl_->last_failure_;
}

LinuxPlatformRuntime::LinuxPlatformRuntime(const LinuxPlatformRuntimeConfig& config,
                                           MetricRegistry* metrics)
    : impl_(new Impl(config, metrics)) {}

}  // namespace eavp
