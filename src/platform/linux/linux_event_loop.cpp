#include "linux_event_loop.hpp"

#include <cerrno>
#include <map>
#include <new>
#include <poll.h>
#include <set>
#include <utility>

#include "eavp/platform/linux/wait_source.hpp"

namespace eavp {
namespace detail {
namespace {

const std::uint64_t kControlToken = 0U;
const int kInfiniteTimeoutMs = -1;
const std::uint64_t kMaxConsecutiveInterruptions = 64U;

Status system_error(const char* message, const char* operation, int error) {
    return Status(StatusCode::kIoError, message, "linux_runtime", operation, error);
}

Status allocation_error() { return Status(StatusCode::kResourceExhausted); }
Status internal_error() { return Status(StatusCode::kInternal); }

std::uint32_t epoll_interests(short events) {
    std::uint32_t interests = 0U;
    if ((events & POLLIN) != 0) interests |= EPOLLIN;
    if ((events & POLLOUT) != 0) interests |= EPOLLOUT;
    if ((events & POLLPRI) != 0) interests |= EPOLLPRI;
    return interests;
}

short poll_events(std::uint32_t events) {
    short translated = 0;
    if ((events & EPOLLIN) != 0U) translated |= POLLIN;
    if ((events & EPOLLOUT) != 0U) translated |= POLLOUT;
    if ((events & EPOLLPRI) != 0U) translated |= POLLPRI;
    if ((events & EPOLLERR) != 0U) translated |= POLLERR;
    if ((events & EPOLLHUP) != 0U) translated |= POLLHUP;
    return translated;
}

class ScopedLoopFds {
public:
    explicit ScopedLoopFds(LinuxRuntimeApi* runtime_api)
        : epoll_fd(-1), event_fd(-1), api_(runtime_api) {}

    ~ScopedLoopFds() noexcept {
        close_noexcept(event_fd);
        close_noexcept(epoll_fd);
    }

    void release() {
        epoll_fd = -1;
        event_fd = -1;
    }

    int epoll_fd;
    int event_fd;

private:
    void close_noexcept(int fd) noexcept {
        if (fd < 0) return;
        try {
            api_->close_fd(fd);
        } catch (...) {
        }
    }

    LinuxRuntimeApi* api_;
};

}  // namespace

class LinuxEventLoop::Impl {
public:
    struct SourceRecord {
        MediaPipeline* pipeline;
        LinuxWaitSource* source;
        std::vector<struct pollfd> descriptors;
        std::vector<std::uint64_t> tokens;
    };

    struct TokenTarget {
        std::size_t source_index;
        std::size_t descriptor_index;
    };

    explicit Impl(std::unique_ptr<LinuxRuntimeApi> runtime_api)
        : api(std::move(runtime_api)), epoll_fd(-1), event_fd(-1), next_token(1U),
          invalidated(false), wait_failure_origin(
                                  LinuxEventLoopWaitFailureOrigin::kNone),
          sources(), tokens(), registered_fds(), registered_sources() {}

    bool rollback_registration(const SourceRecord& record,
                               std::size_t added_count) noexcept {
        bool complete = true;
        while (added_count > 0U) {
            --added_count;
            try {
                if (api->epoll_remove(epoll_fd,
                                      record.descriptors[added_count].fd) < 0) {
                    complete = false;
                }
            } catch (...) {
                complete = false;
            }
        }
        if (!complete) invalidate_noexcept();
        return complete;
    }

    void invalidate_noexcept() noexcept {
        invalidated = true;
        const int owned_event_fd = event_fd;
        const int owned_epoll_fd = epoll_fd;
        event_fd = -1;
        epoll_fd = -1;
        try {
            if (owned_event_fd >= 0) api->close_fd(owned_event_fd);
        } catch (...) {
        }
        try {
            if (owned_epoll_fd >= 0) api->close_fd(owned_epoll_fd);
        } catch (...) {
        }
    }

    std::unique_ptr<LinuxRuntimeApi> api;
    int epoll_fd;
    int event_fd;
    std::uint64_t next_token;
    bool invalidated;
    LinuxEventLoopWaitFailureOrigin wait_failure_origin;
    std::vector<SourceRecord> sources;
    std::map<std::uint64_t, TokenTarget> tokens;
    std::set<int> registered_fds;
    std::set<LinuxWaitSource*> registered_sources;
};

LinuxEventLoop::LinuxEventLoop(std::unique_ptr<LinuxRuntimeApi> api)
    : impl_(new Impl(std::move(api))) {}

LinuxEventLoop::~LinuxEventLoop() noexcept {
    try {
        close();
    } catch (...) {
    }
}

Status LinuxEventLoop::initialize() {
    try {
        if (!impl_->api.get()) return Status(StatusCode::kInvalidArgument);
        if (impl_->invalidated) return Status(StatusCode::kInvalidState);
        if (impl_->epoll_fd >= 0 && impl_->event_fd >= 0) {
            return Status::ok_status();
        }
        if (impl_->epoll_fd >= 0 || impl_->event_fd >= 0) {
            return Status(StatusCode::kInvalidState);
        }

        ScopedLoopFds owned(impl_->api.get());

        owned.epoll_fd = impl_->api->epoll_create();
        if (owned.epoll_fd < 0) {
            const int error = impl_->api->last_error();
            return system_error("无法创建 Linux epoll 实例", "epoll_create1",
                                error);
        }

        owned.event_fd = impl_->api->create_event_fd();
        if (owned.event_fd < 0) {
            const int error = impl_->api->last_error();
            return system_error("无法创建 Linux eventfd", "eventfd", error);
        }

        if (impl_->api->epoll_add(owned.epoll_fd, owned.event_fd, EPOLLIN,
                                  kControlToken) < 0) {
            const int error = impl_->api->last_error();
            const Status failure = system_error(
                "无法向 Linux epoll 注册 eventfd", "epoll_ctl",
                error);
            return failure;
        }
        impl_->epoll_fd = owned.epoll_fd;
        impl_->event_fd = owned.event_fd;
        owned.release();
        return Status::ok_status();
    } catch (const std::bad_alloc&) {
        return allocation_error();
    } catch (...) {
        return internal_error();
    }
}

Status LinuxEventLoop::register_source(MediaPipeline* pipeline,
                                       LinuxWaitSource* source) {
    try {
        if (impl_->invalidated || impl_->epoll_fd < 0 || impl_->event_fd < 0) {
            return Status(StatusCode::kInvalidState);
        }
        if (pipeline == NULL || source == NULL) {
            return Status(StatusCode::kInvalidArgument);
        }
        if (impl_->registered_sources.count(source) != 0U) {
            return Status(StatusCode::kAlreadyExists);
        }

        Result<std::vector<struct pollfd> > descriptors_result =
            source->poll_descriptors();
        if (!descriptors_result.ok()) return descriptors_result.status();
        std::vector<struct pollfd> descriptors = descriptors_result.take_value();
        if (descriptors.empty()) return Status(StatusCode::kInvalidArgument);

        std::set<int> local_fds;
        const short allowed = static_cast<short>(POLLIN | POLLOUT | POLLPRI);
        for (std::size_t index = 0U; index < descriptors.size(); ++index) {
            const short requested = descriptors[index].events;
            if (descriptors[index].fd < 0 || requested == 0 ||
                (requested & static_cast<short>(~allowed)) != 0) {
                return Status(StatusCode::kInvalidArgument);
            }
            if (!local_fds.insert(descriptors[index].fd).second ||
                impl_->registered_fds.count(descriptors[index].fd) != 0U) {
                return Status(StatusCode::kAlreadyExists);
            }
            descriptors[index].revents = 0;
        }

        Impl::SourceRecord record;
        record.pipeline = pipeline;
        record.source = source;
        record.descriptors = descriptors;
        record.tokens.reserve(descriptors.size());
        std::uint64_t staged_next_token = impl_->next_token;
        for (std::size_t index = 0U; index < descriptors.size(); ++index) {
            record.tokens.push_back(staged_next_token++);
        }

        // 先在副本中完成所有分配，内核注册成功后仅以无分配 swap 提交，
        // 避免内核与内存状态因异常分叉。
        std::vector<Impl::SourceRecord> staged_sources = impl_->sources;
        std::map<std::uint64_t, Impl::TokenTarget> staged_tokens = impl_->tokens;
        std::set<int> staged_registered_fds = impl_->registered_fds;
        std::set<LinuxWaitSource*> staged_registered_sources =
            impl_->registered_sources;

        const std::size_t source_index = staged_sources.size();
        staged_sources.push_back(record);
        for (std::size_t index = 0U; index < descriptors.size(); ++index) {
            const Impl::TokenTarget target = {source_index, index};
            staged_tokens.insert(std::make_pair(record.tokens[index], target));
            staged_registered_fds.insert(descriptors[index].fd);
        }
        staged_registered_sources.insert(source);

        std::size_t added_count = 0U;
        try {
            for (std::size_t index = 0U; index < descriptors.size(); ++index) {
                if (impl_->api->epoll_add(
                        impl_->epoll_fd, descriptors[index].fd,
                        epoll_interests(descriptors[index].events),
                        record.tokens[index]) < 0) {
                    const int error = impl_->api->last_error();
                    impl_->rollback_registration(record, added_count);
                    added_count = 0U;
                    const Status failure = system_error(
                        "无法向 Linux epoll 注册 descriptor", "epoll_ctl",
                        error);
                    return failure;
                }
                ++added_count;
            }

            impl_->sources.swap(staged_sources);
            impl_->tokens.swap(staged_tokens);
            impl_->registered_fds.swap(staged_registered_fds);
            impl_->registered_sources.swap(staged_registered_sources);
            impl_->next_token = staged_next_token;
            return Status::ok_status();
        } catch (const std::bad_alloc&) {
            impl_->rollback_registration(record, added_count);
            return allocation_error();
        } catch (...) {
            impl_->rollback_registration(record, added_count);
            return internal_error();
        }
    } catch (const std::bad_alloc&) {
        return allocation_error();
    } catch (...) {
        return internal_error();
    }
}

Result<LinuxEventLoopTurn> LinuxEventLoop::wait_once() {
    impl_->wait_failure_origin = LinuxEventLoopWaitFailureOrigin::kNone;
    try {
        if (impl_->epoll_fd < 0 || impl_->event_fd < 0) {
            impl_->wait_failure_origin =
                LinuxEventLoopWaitFailureOrigin::kRuntime;
            return Result<LinuxEventLoopTurn>(Status(StatusCode::kInvalidState));
        }

        LinuxEventLoopTurn turn;
        std::vector<struct epoll_event> events(impl_->registered_fds.size() + 1U);
        int ready_count = -1;
        while (true) {
            ready_count = impl_->api->epoll_wait_events(
                impl_->epoll_fd, &events[0], static_cast<int>(events.size()),
                kInfiniteTimeoutMs);
            if (ready_count >= 0) {
                turn.wakeup_count = 1U;
                break;
            }
            const int error = impl_->api->last_error();
            if (error != EINTR) {
                impl_->wait_failure_origin =
                    LinuxEventLoopWaitFailureOrigin::kRuntime;
                return Result<LinuxEventLoopTurn>(system_error(
                    "Linux Reactor 等待失败", "epoll_wait", error));
            }
            ++turn.interrupted_count;
            if (turn.interrupted_count >= kMaxConsecutiveInterruptions) {
                impl_->wait_failure_origin =
                    LinuxEventLoopWaitFailureOrigin::kRuntime;
                return Result<LinuxEventLoopTurn>(Status(
                    StatusCode::kIoError,
                    "Linux Reactor 等待被连续信号中断",
                    "linux_runtime", "epoll_wait", EINTR));
            }
        }

        for (std::size_t source_index = 0U;
             source_index < impl_->sources.size(); ++source_index) {
            std::vector<struct pollfd>& descriptors =
                impl_->sources[source_index].descriptors;
            for (std::size_t descriptor_index = 0U;
                 descriptor_index < descriptors.size(); ++descriptor_index) {
                descriptors[descriptor_index].revents = 0;
            }
        }

        std::set<std::size_t> ready_sources;
        for (int event_index = 0; event_index < ready_count; ++event_index) {
            const std::uint64_t token = events[static_cast<std::size_t>(event_index)].data.u64;
            if (token == kControlToken) {
                if (!turn.control_wakeup) {
                    std::uint64_t value = 0U;
                    if (impl_->api->read_event_fd(impl_->event_fd, &value) < 0) {
                        impl_->wait_failure_origin =
                            LinuxEventLoopWaitFailureOrigin::kRuntime;
                        return Result<LinuxEventLoopTurn>(system_error(
                            "无法读取 Linux eventfd", "read(eventfd)",
                            impl_->api->last_error()));
                    }
                    turn.control_wakeup = true;
                }
                continue;
            }

            const std::map<std::uint64_t, Impl::TokenTarget>::const_iterator found =
                impl_->tokens.find(token);
            if (found == impl_->tokens.end()) continue;
            Impl::SourceRecord& source = impl_->sources[found->second.source_index];
            source.descriptors[found->second.descriptor_index].revents |=
                poll_events(events[static_cast<std::size_t>(event_index)].events);
            ready_sources.insert(found->second.source_index);
        }

        std::set<MediaPipeline*> ready_pipelines;
        for (std::set<std::size_t>::const_iterator source_index =
                 ready_sources.begin();
             source_index != ready_sources.end(); ++source_index) {
            Impl::SourceRecord& source = impl_->sources[*source_index];
            Result<bool> evaluated =
                source.source->evaluate_poll_events(source.descriptors);
            if (!evaluated.ok()) {
                impl_->wait_failure_origin =
                    LinuxEventLoopWaitFailureOrigin::kWaitSource;
                return Result<LinuxEventLoopTurn>(evaluated.status());
            }
            if (evaluated.value() &&
                ready_pipelines.insert(source.pipeline).second) {
                turn.ready_pipelines.push_back(source.pipeline);
            }
        }
        return Result<LinuxEventLoopTurn>(std::move(turn));
    } catch (const std::bad_alloc&) {
        impl_->wait_failure_origin =
            LinuxEventLoopWaitFailureOrigin::kRuntime;
        return Result<LinuxEventLoopTurn>(allocation_error());
    } catch (...) {
        impl_->wait_failure_origin =
            LinuxEventLoopWaitFailureOrigin::kRuntime;
        return Result<LinuxEventLoopTurn>(internal_error());
    }
}

LinuxEventLoopWaitFailureOrigin LinuxEventLoop::wait_failure_origin() const {
    return impl_->wait_failure_origin;
}

Status LinuxEventLoop::wake() {
    try {
        if (impl_->event_fd < 0) return Status(StatusCode::kInvalidState);
        if (impl_->api->write_event_fd(impl_->event_fd, 1U) < 0) {
            return system_error("无法写入 Linux eventfd", "write(eventfd)",
                                impl_->api->last_error());
        }
        return Status::ok_status();
    } catch (...) {
        return internal_error();
    }
}

Status LinuxEventLoop::close() {
    try {
        Status first_failure = Status::ok_status();
        if (impl_->event_fd >= 0) {
            const int event_fd = impl_->event_fd;
            impl_->event_fd = -1;
            if (impl_->api->close_fd(event_fd) < 0) {
                first_failure = system_error(
                    "无法关闭 Linux eventfd", "close(eventfd)",
                    impl_->api->last_error());
            }
        }
        if (impl_->epoll_fd >= 0) {
            const int epoll_fd = impl_->epoll_fd;
            impl_->epoll_fd = -1;
            if (impl_->api->close_fd(epoll_fd) < 0 && first_failure.ok()) {
                first_failure = system_error(
                    "无法关闭 Linux epoll 实例", "close(epoll)",
                    impl_->api->last_error());
            }
        }
        return first_failure;
    } catch (...) {
        return internal_error();
    }
}

}  // namespace detail
}  // namespace eavp
