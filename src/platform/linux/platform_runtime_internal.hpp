#ifndef EAVP_PLATFORM_LINUX_PLATFORM_RUNTIME_INTERNAL_HPP_
#define EAVP_PLATFORM_LINUX_PLATFORM_RUNTIME_INTERNAL_HPP_

#include <cstdint>
#include <memory>

#include "eavp/platform/linux/platform_runtime.hpp"
#include "linux_runtime_api.hpp"

namespace eavp {
namespace detail {

class RuntimeObserver {
public:
    virtual ~RuntimeObserver() {}

    virtual Status on_poll(std::uint64_t wakeups,
                           std::uint64_t interrupted) = 0;
    virtual Status on_pipeline_turn() = 0;
    virtual Status on_pipeline_failure() = 0;
    virtual Status on_reactor_running(bool running) = 0;
};

class RuntimeTestHooks {
public:
    virtual ~RuntimeTestHooks() {}

    virtual void on_wake_claimed() {}
    virtual void on_reactor_close_pending() {}
    virtual void on_reactor_waiting_for_wake() {}
    virtual void on_join_owner_claimed() {}
    virtual void on_join_waiter() {}
    virtual void on_startup_result_ready() {}
    virtual void on_reactor_thread_finishing() {}
    virtual PlatformRuntimeState snapshot_state(PlatformRuntimeState state) {
        return state;
    }
    virtual Status snapshot_last_failure(const Status& failure) {
        return failure;
    }
};

class LinuxPlatformRuntimeTestPeer {
public:
    static Result<std::unique_ptr<LinuxPlatformRuntime> > create(
        const LinuxPlatformRuntimeConfig& config,
        std::unique_ptr<LinuxRuntimeApi> api,
        // observer 仅由测试调用方持有，必须晚于 Runtime 销毁。
        RuntimeObserver* observer = NULL,
        // hooks 仅用于确定性并发与异常边界测试，生命周期同 observer。
        RuntimeTestHooks* hooks = NULL);
};

}  // namespace detail
}  // namespace eavp

#endif  // EAVP_PLATFORM_LINUX_PLATFORM_RUNTIME_INTERNAL_HPP_
