#ifndef EAVP_PLATFORM_LINUX_PLATFORM_RUNTIME_HPP_
#define EAVP_PLATFORM_LINUX_PLATFORM_RUNTIME_HPP_

#include <memory>
#include <vector>

#include "eavp/base/result.hpp"
#include "eavp/platform/linux/wait_source.hpp"

namespace eavp {

class MediaPipeline;
class MetricRegistry;

class LinuxPlatformRuntimeConfig {
public:
    static Result<LinuxPlatformRuntimeConfig> create(int reactor_count,
                                                      int stop_timeout_ms);

    int reactor_count() const { return reactor_count_; }
    int stop_timeout_ms() const { return stop_timeout_ms_; }

private:
    LinuxPlatformRuntimeConfig(int reactor_count, int stop_timeout_ms)
        : reactor_count_(reactor_count), stop_timeout_ms_(stop_timeout_ms) {}

    int reactor_count_;
    int stop_timeout_ms_;
};

enum class PlatformRuntimeState {
    kCreated,
    kStarting,
    kRunning,
    kStopping,
    kStopped,
    kError,
};

class LinuxPlatformRuntime {
public:
    // metrics 可为空；为空时仅关闭 Runtime Metrics，不影响调度或错误传播。
    static Result<std::unique_ptr<LinuxPlatformRuntime> > create(
        const LinuxPlatformRuntimeConfig& config, MetricRegistry* metrics);
    ~LinuxPlatformRuntime() noexcept;

    // Runtime 不取得 MediaPipeline 或 LinuxWaitSource 的所有权。调用方必须先完成
    // stop() 并销毁 Runtime，随后才能销毁这些被借用对象。
    Status register_pipeline(MediaPipeline* pipeline,
                             const std::vector<LinuxWaitSource*>& wait_sources);
    Status start();
    Status stop();
    PlatformRuntimeState state() const;
    Status last_failure() const;

private:
    class Impl;

    LinuxPlatformRuntime(const LinuxPlatformRuntimeConfig& config,
                         MetricRegistry* metrics);

    std::unique_ptr<Impl> impl_;
};

}  // namespace eavp

#endif  // EAVP_PLATFORM_LINUX_PLATFORM_RUNTIME_HPP_
