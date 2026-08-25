#ifndef EAVP_TESTS_SUPPORT_FAKE_LINUX_RUNTIME_API_HPP_
#define EAVP_TESTS_SUPPORT_FAKE_LINUX_RUNTIME_API_HPP_

#include <cerrno>
#include <cstdint>
#include <deque>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <vector>

#include "../../src/platform/linux/linux_runtime_api.hpp"

namespace eavp_test {

class FakeLinuxRuntimeApi : public eavp::detail::LinuxRuntimeApi {
public:
    struct AddCall {
        int epoll_fd;
        int fd;
        std::uint32_t events;
        std::uint64_t token;
    };

    struct ReadyEvent {
        ReadyEvent(int ready_fd, std::uint32_t ready_events)
            : fd(ready_fd), events(ready_events), spurious(false), token(0U) {}

        static ReadyEvent spurious_token(std::uint64_t value,
                                         std::uint32_t events) {
            ReadyEvent event(-1, events);
            event.spurious = true;
            event.token = value;
            return event;
        }

        int fd;
        std::uint32_t events;
        bool spurious;
        std::uint64_t token;
    };

    struct RemoveCall {
        int epoll_fd;
        int fd;
    };

    explicit FakeLinuxRuntimeApi(std::vector<int>* external_closed_fds = NULL)
        : epoll_fd_result(40), event_fd_result(41), epoll_add_result(0),
          epoll_remove_result(0), fail_epoll_add_call(0),
          fail_epoll_add_error(EIO), epoll_remove_error(ENOENT),
          throw_on_epoll_create(false), throw_on_create_event_fd(false),
          throw_on_epoll_add_call(0), throw_bad_alloc_on_epoll_add_call(0),
          read_event_fd_result(0), write_event_fd_result(0), close_fd_result(0),
          monotonic_now_result(0), saved_error(EIO), event_fd_value(1U),
          epoll_create_count(0), create_event_fd_count(0), epoll_add_count(0),
          epoll_wait_count(0),
          read_event_fd_count(0), write_event_fd_count(0),
          monotonic_now_count(0), written_event_fd(-1), written_value(0U),
          add_calls(), remove_calls(), closed_fds(),
          external_closed_fds_(external_closed_fds), registered_fds_(),
          wait_steps() {}

    int epoll_create() {
        ++epoll_create_count;
        if (throw_on_epoll_create) throw std::runtime_error("epoll_create");
        return epoll_fd_result;
    }

    int epoll_add(int epoll_fd, int fd, std::uint32_t events,
                  std::uint64_t token) {
        AddCall call = {epoll_fd, fd, events, token};
        add_calls.push_back(call);
        ++epoll_add_count;
        if (epoll_add_count == throw_bad_alloc_on_epoll_add_call) {
            throw std::bad_alloc();
        }
        if (epoll_add_count == throw_on_epoll_add_call) {
            throw std::runtime_error("epoll_add");
        }
        if (epoll_add_count == fail_epoll_add_call) {
            saved_error = fail_epoll_add_error;
            return -1;
        }
        if (epoll_add_result < 0) return epoll_add_result;
        if (registered_fds_.count(fd) != 0U) {
            saved_error = EEXIST;
            return -1;
        }
        registered_fds_.insert(fd);
        return epoll_add_result;
    }

    int epoll_remove(int epoll_fd, int fd) {
        const RemoveCall call = {epoll_fd, fd};
        remove_calls.push_back(call);
        if (epoll_remove_result < 0) {
            saved_error = epoll_remove_error;
            return -1;
        }
        registered_fds_.erase(fd);
        return 0;
    }

    int epoll_wait_events(int, struct epoll_event* events, int capacity, int) {
        ++epoll_wait_count;
        if (wait_steps.empty()) return 0;
        const WaitStep step = wait_steps.front();
        wait_steps.pop_front();
        if (step.result < 0) {
            saved_error = step.error;
            return -1;
        }
        const int count = static_cast<int>(step.events.size());
        if (count > capacity) return -1;
        for (int index = 0; index < count; ++index) {
            events[index].events = step.events[static_cast<std::size_t>(index)].events;
            events[index].data.u64 = token_for(step.events[static_cast<std::size_t>(index)]);
        }
        return count;
    }

    int create_event_fd() {
        ++create_event_fd_count;
        if (throw_on_create_event_fd) throw std::bad_alloc();
        return event_fd_result;
    }

    int read_event_fd(int, std::uint64_t* value) {
        ++read_event_fd_count;
        if (read_event_fd_result == 0) *value = event_fd_value;
        return read_event_fd_result;
    }

    int write_event_fd(int fd, std::uint64_t value) {
        ++write_event_fd_count;
        written_event_fd = fd;
        written_value = value;
        return write_event_fd_result;
    }

    int close_fd(int fd) {
        closed_fds.push_back(fd);
        if (fd == epoll_fd_result) registered_fds_.clear();
        if (external_closed_fds_ != NULL) external_closed_fds_->push_back(fd);
        return close_fd_result;
    }

    int monotonic_now(struct timespec* value) {
        ++monotonic_now_count;
        if (monotonic_now_result == 0) {
            value->tv_sec = 0;
            value->tv_nsec = 0L;
        }
        return monotonic_now_result;
    }

    int last_error() const { return saved_error; }

    void queue_ready_fds(const std::vector<int>& fds) {
        std::vector<ReadyEvent> events;
        for (std::size_t index = 0U; index < fds.size(); ++index) {
            events.push_back(ReadyEvent(fds[index], EPOLLIN));
        }
        queue_events(events);
    }

    void queue_events(const std::vector<ReadyEvent>& events) {
        WaitStep step;
        step.result = static_cast<int>(events.size());
        step.error = 0;
        step.events = events;
        wait_steps.push_back(step);
    }

    void queue_error(int error) {
        WaitStep step;
        step.result = -1;
        step.error = error;
        wait_steps.push_back(step);
    }

    int close_count_for(int fd) const {
        int count = 0;
        for (std::size_t index = 0U; index < closed_fds.size(); ++index) {
            if (closed_fds[index] == fd) ++count;
        }
        return count;
    }

    int epoll_fd_result;
    int event_fd_result;
    int epoll_add_result;
    int epoll_remove_result;
    int fail_epoll_add_call;
    int fail_epoll_add_error;
    int epoll_remove_error;
    bool throw_on_epoll_create;
    bool throw_on_create_event_fd;
    int throw_on_epoll_add_call;
    int throw_bad_alloc_on_epoll_add_call;
    int read_event_fd_result;
    int write_event_fd_result;
    int close_fd_result;
    int monotonic_now_result;
    int saved_error;
    std::uint64_t event_fd_value;
    int epoll_create_count;
    int create_event_fd_count;
    int epoll_add_count;
    int epoll_wait_count;
    int read_event_fd_count;
    int write_event_fd_count;
    int monotonic_now_count;
    int written_event_fd;
    std::uint64_t written_value;
    std::vector<AddCall> add_calls;
    std::vector<RemoveCall> remove_calls;
    std::vector<int> closed_fds;

private:
    struct WaitStep {
        int result;
        int error;
        std::vector<ReadyEvent> events;
    };

    std::uint64_t token_for(const ReadyEvent& event) const {
        if (event.spurious) return event.token;
        for (std::size_t index = 0U; index < add_calls.size(); ++index) {
            if (add_calls[index].fd == event.fd) return add_calls[index].token;
        }
        return std::numeric_limits<std::uint64_t>::max();
    }

    std::vector<int>* external_closed_fds_;
    std::set<int> registered_fds_;
    std::deque<WaitStep> wait_steps;
};

}  // namespace eavp_test

#endif  // EAVP_TESTS_SUPPORT_FAKE_LINUX_RUNTIME_API_HPP_
