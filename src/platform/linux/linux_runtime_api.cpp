#include "linux_runtime_api.hpp"

#include <cerrno>
#include <sys/eventfd.h>
#include <unistd.h>

namespace eavp {
namespace detail {
namespace {

class PosixLinuxRuntimeApi : public LinuxRuntimeApi {
public:
    PosixLinuxRuntimeApi() : last_error_(0) {}

    int epoll_create() {
        const int result = ::epoll_create1(EPOLL_CLOEXEC);
        return save_failure(result);
    }

    int epoll_add(int epoll_fd, int fd, std::uint32_t events,
                  std::uint64_t token) {
        struct epoll_event event;
        event.events = events;
        event.data.u64 = token;
        const int result = ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event);
        return save_failure(result);
    }

    int epoll_wait_events(int epoll_fd, struct epoll_event* events,
                          int capacity, int timeout_ms) {
        const int result = ::epoll_wait(epoll_fd, events, capacity, timeout_ms);
        return save_failure(result);
    }

    int create_event_fd() {
        const int result = ::eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC);
        return save_failure(result);
    }

    int read_event_fd(int fd, std::uint64_t* value) {
        const ssize_t result = ::read(fd, value, sizeof(*value));
        if (result == static_cast<ssize_t>(sizeof(*value))) return 0;
        last_error_ = result < 0 ? errno : EIO;
        return -1;
    }

    int write_event_fd(int fd, std::uint64_t value) {
        const ssize_t result = ::write(fd, &value, sizeof(value));
        if (result == static_cast<ssize_t>(sizeof(value))) return 0;
        last_error_ = result < 0 ? errno : EIO;
        return -1;
    }

    int close_fd(int fd) { return save_failure(::close(fd)); }

    int monotonic_now(struct timespec* value) {
        return save_failure(::clock_gettime(CLOCK_MONOTONIC, value));
    }

    int last_error() const { return last_error_; }

private:
    int save_failure(int result) {
        if (result < 0) last_error_ = errno;
        return result;
    }

    int last_error_;
};

}  // namespace

std::unique_ptr<LinuxRuntimeApi> create_linux_runtime_api() {
    return std::unique_ptr<LinuxRuntimeApi>(new PosixLinuxRuntimeApi());
}

}  // namespace detail
}  // namespace eavp
