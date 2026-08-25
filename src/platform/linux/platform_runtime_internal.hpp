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

class LinuxPlatformRuntimeTestPeer {
public:
    static Result<std::unique_ptr<LinuxPlatformRuntime> > create(
        const LinuxPlatformRuntimeConfig& config,
        std::unique_ptr<LinuxRuntimeApi> api,
        // observer 仅由测试调用方持有，必须晚于 Runtime 销毁。
        RuntimeObserver* observer = NULL);
};

}  // namespace detail
}  // namespace eavp

#endif  // EAVP_PLATFORM_LINUX_PLATFORM_RUNTIME_INTERNAL_HPP_
