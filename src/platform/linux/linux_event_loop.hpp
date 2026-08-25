#ifndef EAVP_PLATFORM_LINUX_LINUX_EVENT_LOOP_HPP_
#define EAVP_PLATFORM_LINUX_LINUX_EVENT_LOOP_HPP_

#include <cstdint>
#include <memory>
#include <vector>

#include "eavp/base/result.hpp"
#include "linux_runtime_api.hpp"

namespace eavp {

class LinuxWaitSource;
class MediaPipeline;

namespace detail {

struct LinuxEventLoopTurn {
    LinuxEventLoopTurn()
        : ready_pipelines(), wakeup_count(0U), interrupted_count(0U),
          control_wakeup(false) {}

    std::vector<MediaPipeline*> ready_pipelines;
    std::uint64_t wakeup_count;
    std::uint64_t interrupted_count;
    bool control_wakeup;
};

class LinuxEventLoop {
public:
    explicit LinuxEventLoop(std::unique_ptr<LinuxRuntimeApi> api);
    ~LinuxEventLoop() noexcept;

    Status initialize();
    Status register_source(MediaPipeline* pipeline, LinuxWaitSource* source);
    Result<LinuxEventLoopTurn> wait_once();
    Status wake();
    Status close();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace detail
}  // namespace eavp

#endif  // EAVP_PLATFORM_LINUX_LINUX_EVENT_LOOP_HPP_
