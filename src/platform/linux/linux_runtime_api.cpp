#include "linux_runtime_api.hpp"

#include <cerrno>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

namespace eavp {
namespace detail {
namespace {

int& last_error_slot() {
    // wait 与 wake 可位于不同线程；errno 快照必须和发起 syscall 的线程绑定。
    static thread_local int error = 0;
    return error;
}

class PosixLinuxRuntimeApi : public LinuxRuntimeApi {
public:
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

    int epoll_remove(int epoll_fd, int fd) {
        return save_failure(::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL));
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
        last_error_slot() = result < 0 ? errno : EIO;
        return -1;
    }

    int write_event_fd(int fd, std::uint64_t value) {
        const ssize_t result = ::write(fd, &value, sizeof(value));
        if (result == static_cast<ssize_t>(sizeof(value))) return 0;
        last_error_slot() = result < 0 ? errno : EIO;
        return -1;
    }

    int close_fd(int fd) { return save_failure(::close(fd)); }

    int monotonic_now(struct timespec* value) {
        return save_failure(::clock_gettime(CLOCK_MONOTONIC, value));
    }

    int monotonic_sleep_until(const struct timespec* deadline) {
        const int result = ::clock_nanosleep(
            CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, NULL);
        if (result == 0) return 0;
        last_error_slot() = result;
        return -1;
    }

    int last_error() const { return last_error_slot(); }

private:
    int save_failure(int result) {
        if (result < 0) last_error_slot() = errno;
        return result;
    }
};

}  // namespace

std::unique_ptr<LinuxRuntimeApi> create_linux_runtime_api() {
    return std::unique_ptr<LinuxRuntimeApi>(new PosixLinuxRuntimeApi());
}

}  // namespace detail
}  // namespace eavp
