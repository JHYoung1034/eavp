#ifndef EAVP_PLATFORM_LINUX_LINUX_RUNTIME_API_HPP_
#define EAVP_PLATFORM_LINUX_LINUX_RUNTIME_API_HPP_

#include <cstdint>
#include <ctime>
#include <memory>
#include <sys/epoll.h>

namespace eavp {
namespace detail {

class LinuxRuntimeApi {
public:
    virtual ~LinuxRuntimeApi() {}

    virtual int epoll_create() = 0;
    virtual int epoll_add(int epoll_fd, int fd, std::uint32_t events,
                          std::uint64_t token) = 0;
    virtual int epoll_remove(int epoll_fd, int fd) = 0;
    virtual int epoll_wait_events(int epoll_fd, struct epoll_event* events,
                                  int capacity, int timeout_ms) = 0;
    virtual int create_event_fd() = 0;
    virtual int read_event_fd(int fd, std::uint64_t* value) = 0;
    virtual int write_event_fd(int fd, std::uint64_t value) = 0;
    virtual int close_fd(int fd) = 0;
    virtual int monotonic_now(struct timespec* value) = 0;
    virtual int monotonic_sleep_until(const struct timespec* deadline) = 0;
    virtual int last_error() const = 0;
};

std::unique_ptr<LinuxRuntimeApi> create_linux_runtime_api();

}  // namespace detail
}  // namespace eavp

#endif  // EAVP_PLATFORM_LINUX_LINUX_RUNTIME_API_HPP_
