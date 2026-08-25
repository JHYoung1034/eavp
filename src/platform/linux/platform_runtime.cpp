#include "eavp/platform/linux/platform_runtime.hpp"

#include <condition_variable>
#include <ctime>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "eavp/management/metrics.hpp"
#include "eavp/media/pipeline.hpp"
#include "linux_event_loop.hpp"
#include "platform_runtime_internal.hpp"

namespace eavp {
namespace {

Status allocation_failure() {
    return Status(StatusCode::kResourceExhausted);
}

Status internal_failure() {
    return Status(StatusCode::kInternal);
}

void record_first(const Status& candidate, Status* first_failure) {
    if (first_failure->ok() && !candidate.ok()) *first_failure = candidate;
}

Status selected_failure(const Status& media_failure,
                        const Status& reactor_failure,
                        const Status& observer_failure) {
    if (!media_failure.ok()) return media_failure;
    if (!reactor_failure.ok()) return reactor_failure;
    return observer_failure;
}

Status clock_failure(detail::LinuxRuntimeApi* api) {
    return Status(StatusCode::kIoError, "无法读取 Linux 单调时钟",
                  "linux_runtime", "clock_gettime", api->last_error());
}

void add_milliseconds(const struct timespec& start, int milliseconds,
                      struct timespec* deadline) {
    deadline->tv_sec = start.tv_sec + milliseconds / 1000;
    deadline->tv_nsec =
        start.tv_nsec + static_cast<long>(milliseconds % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000L;
    }
}

bool reached(const struct timespec& now, const struct timespec& deadline) {
    return now.tv_sec > deadline.tv_sec ||
           (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec);
}

template <typename ObserverCall>
Status invoke_observer(const ObserverCall& call) {
    try {
        return call();
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    } catch (...) {
        return internal_failure();
    }
}

class NullRuntimeObserver : public detail::RuntimeObserver {
public:
    Status on_poll(std::uint64_t, std::uint64_t) override {
        return Status::ok_status();
    }
    Status on_pipeline_turn() override { return Status::ok_status(); }
    Status on_pipeline_failure() override { return Status::ok_status(); }
    Status on_reactor_running(bool) override { return Status::ok_status(); }
};

class RegistryRuntimeObserver : public detail::RuntimeObserver {
public:
    explicit RegistryRuntimeObserver(MetricRegistry* metrics)
        : metrics_(metrics) {}

    Status on_poll(std::uint64_t wakeups,
                   std::uint64_t interrupted) override {
        Status result = metrics_->increment_counter(
            "runtime.poll.wakeups", wakeups);
        record_first(metrics_->increment_counter(
                         "runtime.poll.interrupted", interrupted),
                     &result);
        return result;
    }

    Status on_pipeline_turn() override {
        return metrics_->increment_counter("runtime.pipeline.turns");
    }

    Status on_pipeline_failure() override {
        return metrics_->increment_counter("runtime.pipeline.failures");
    }

    Status on_reactor_running(bool running) override {
        return metrics_->set_gauge("runtime.reactor.running",
                                   running ? 1.0 : 0.0);
    }

private:
    MetricRegistry* metrics_;
};

struct TestDependencies {
    TestDependencies(std::unique_ptr<detail::LinuxRuntimeApi> runtime_api,
                     detail::RuntimeObserver* runtime_observer)
        : api(std::move(runtime_api)), observer(runtime_observer) {}

    std::unique_ptr<detail::LinuxRuntimeApi> api;
    detail::RuntimeObserver* observer;
};

TestDependencies*& test_dependencies_slot() {
    static thread_local TestDependencies* dependencies = NULL;
    return dependencies;
}

class ScopedTestDependencies {
public:
    explicit ScopedTestDependencies(TestDependencies* dependencies)
        : previous_(test_dependencies_slot()) {
        test_dependencies_slot() = dependencies;
    }

    ~ScopedTestDependencies() {
        test_dependencies_slot() = previous_;
    }

private:
    TestDependencies* previous_;
};

}  // namespace

class LinuxPlatformRuntime::Impl {
public:
    struct PipelineRecord {
        MediaPipeline* pipeline;
        std::vector<LinuxWaitSource*> wait_sources;
    };

    Impl(const LinuxPlatformRuntimeConfig& config, MetricRegistry* metrics,
         std::unique_ptr<detail::LinuxRuntimeApi> api,
         detail::RuntimeObserver* observer)
        : config_(config), owned_observer_(), observer_(observer),
          runtime_api_(api.get()), loop_(new detail::LinuxEventLoop(std::move(api))),
          state_(PlatformRuntimeState::kCreated),
          last_failure_(StatusCode::kInvalidState),
          stop_result_(Status::ok_status()), thread_exited_(false),
          stop_requested_(false), reactor_exiting_(false), pipelines_(),
          registered_pipelines_(), registered_sources_(), reactor_thread_() {
        if (observer_ == NULL) {
            if (metrics != NULL) {
                owned_observer_.reset(new RegistryRuntimeObserver(metrics));
            } else {
                owned_observer_.reset(new NullRuntimeObserver());
            }
            observer_ = owned_observer_.get();
        }
    }

    Status register_pipeline(
        MediaPipeline* pipeline,
        const std::vector<LinuxWaitSource*>& wait_sources) {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ != PlatformRuntimeState::kCreated) {
                return Status(StatusCode::kInvalidState);
            }
            if (pipeline == NULL || wait_sources.empty()) {
                return Status(StatusCode::kInvalidArgument);
            }
            if (registered_pipelines_.count(pipeline) != 0U) {
                return Status(StatusCode::kAlreadyExists);
            }

            std::set<LinuxWaitSource*> local_sources;
            for (std::size_t index = 0U; index < wait_sources.size(); ++index) {
                LinuxWaitSource* source = wait_sources[index];
                if (source == NULL) {
                    return Status(StatusCode::kInvalidArgument);
                }
                if (!local_sources.insert(source).second ||
                    registered_sources_.count(source) != 0U) {
                    return Status(StatusCode::kAlreadyExists);
                }
            }

            std::vector<PipelineRecord> staged_pipelines = pipelines_;
            std::set<MediaPipeline*> staged_registered_pipelines =
                registered_pipelines_;
            std::set<LinuxWaitSource*> staged_registered_sources =
                registered_sources_;
            const PipelineRecord record = {pipeline, wait_sources};
            staged_pipelines.push_back(record);
            staged_registered_pipelines.insert(pipeline);
            staged_registered_sources.insert(local_sources.begin(),
                                               local_sources.end());
            pipelines_.swap(staged_pipelines);
            registered_pipelines_.swap(staged_registered_pipelines);
            registered_sources_.swap(staged_registered_sources);
            return Status::ok_status();
        } catch (const std::bad_alloc&) {
            return allocation_failure();
        } catch (...) {
            return internal_failure();
        }
    }

    Status start() {
        try {
            std::unique_lock<std::mutex> lock(mutex_);
            if (state_ == PlatformRuntimeState::kRunning) {
                return Status::ok_status();
            }
            if (state_ != PlatformRuntimeState::kCreated) {
                return Status(StatusCode::kInvalidState);
            }
            if (pipelines_.empty()) {
                return Status(StatusCode::kInvalidState);
            }

            state_ = PlatformRuntimeState::kStarting;
            thread_exited_ = false;
            stop_requested_ = false;
            reactor_exiting_ = false;
            stop_result_ = Status::ok_status();
            try {
                reactor_thread_ = std::thread(&Impl::reactor_main, this);
            } catch (const std::bad_alloc&) {
                last_failure_ = allocation_failure();
                stop_result_ = last_failure_;
                state_ = PlatformRuntimeState::kError;
                thread_exited_ = true;
                condition_.notify_all();
                return last_failure_;
            } catch (...) {
                last_failure_ = internal_failure();
                stop_result_ = last_failure_;
                state_ = PlatformRuntimeState::kError;
                thread_exited_ = true;
                condition_.notify_all();
                return last_failure_;
            }

            condition_.wait(lock, [this]() {
                return state_ == PlatformRuntimeState::kRunning ||
                       state_ == PlatformRuntimeState::kError ||
                       thread_exited_;
            });
            const bool started = state_ == PlatformRuntimeState::kRunning;
            const Status result = started ? Status::ok_status() : last_failure_;
            const bool join_failed_start = !started && reactor_thread_.joinable();
            lock.unlock();
            if (join_failed_start) reactor_thread_.join();
            return result;
        } catch (const std::bad_alloc&) {
            join_noexcept();
            return allocation_failure();
        } catch (...) {
            join_noexcept();
            return internal_failure();
        }
    }

    Status stop() {
        try {
            std::unique_lock<std::mutex> lock(mutex_);
            while (state_ == PlatformRuntimeState::kStarting) {
                condition_.wait(lock);
            }
            if (state_ == PlatformRuntimeState::kCreated) {
                state_ = PlatformRuntimeState::kStopped;
                stop_result_ = Status::ok_status();
                return stop_result_;
            }
            if (state_ == PlatformRuntimeState::kStopped) {
                return stop_result_;
            }
            if (state_ == PlatformRuntimeState::kError && thread_exited_) {
                const Status result = stop_result_;
                const bool join_thread = reactor_thread_.joinable();
                lock.unlock();
                if (join_thread) reactor_thread_.join();
                return result;
            }
            if (reactor_exiting_) {
                const bool join_thread = reactor_thread_.joinable();
                lock.unlock();
                if (join_thread) reactor_thread_.join();
                lock.lock();
                return stop_result_;
            }

            stop_requested_ = true;
            state_ = PlatformRuntimeState::kStopping;
            lock.unlock();

            const Status wake_status = loop_->wake();
            lock.lock();
            if (!wake_status.ok() && stop_result_.ok()) {
                stop_result_ = wake_status;
            }
            lock.unlock();

            if (reactor_thread_.joinable()) reactor_thread_.join();

            lock.lock();
            return stop_result_;
        } catch (const std::bad_alloc&) {
            join_noexcept();
            return allocation_failure();
        } catch (...) {
            join_noexcept();
            return internal_failure();
        }
    }

    PlatformRuntimeState state() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    Status last_failure() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_failure_;
    }

    void join_noexcept() noexcept {
        try {
            if (reactor_thread_.joinable()) reactor_thread_.join();
        } catch (...) {
        }
    }

private:
    Status observe_poll(const detail::LinuxEventLoopTurn& turn) {
        return invoke_observer([this, &turn]() {
            return observer_->on_poll(turn.wakeup_count,
                                      turn.interrupted_count);
        });
    }

    Status observe_pipeline_turn() {
        return invoke_observer([this]() {
            return observer_->on_pipeline_turn();
        });
    }

    Status observe_pipeline_failure() {
        return invoke_observer([this]() {
            return observer_->on_pipeline_failure();
        });
    }

    Status observe_running(bool running) {
        return invoke_observer([this, running]() {
            return observer_->on_reactor_running(running);
        });
    }

    void cancel_prefix(std::size_t count, Status* media_failure) noexcept {
        while (count > 0U) {
            --count;
            try {
                record_first(pipelines_[count].pipeline->cancel(), media_failure);
            } catch (...) {
                record_first(internal_failure(), media_failure);
            }
        }
    }

    void cancel_all(Status* media_failure) noexcept {
        cancel_prefix(pipelines_.size(), media_failure);
    }

    void finish(const Status& media_failure,
                const Status& reactor_failure,
                const Status& observer_failure) {
        Status final_failure = selected_failure(
            media_failure, reactor_failure, observer_failure);
        std::lock_guard<std::mutex> lock(mutex_);
        if (media_failure.ok() && !stop_result_.ok()) {
            final_failure = stop_result_;
        }
        thread_exited_ = true;
        stop_result_ = final_failure;
        if (final_failure.ok()) {
            state_ = PlatformRuntimeState::kStopped;
        } else {
            last_failure_ = final_failure;
            state_ = PlatformRuntimeState::kError;
        }
        condition_.notify_all();
    }

    void fail_startup(Status media_failure, Status reactor_failure,
                      Status observer_failure, std::size_t attempted_count,
                      bool running_was_observed) {
        cancel_prefix(attempted_count, &media_failure);
        record_first(loop_->close(), &reactor_failure);
        if (running_was_observed) {
            record_first(observe_running(false), &observer_failure);
        }
        finish(media_failure, reactor_failure, observer_failure);
    }

    bool stop_requested() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stop_requested_;
    }

    void mark_reactor_exiting() {
        std::lock_guard<std::mutex> lock(mutex_);
        reactor_exiting_ = true;
    }

    void drain(Status* media_failure, Status* reactor_failure,
               Status* observer_failure) {
        struct timespec started;
        if (runtime_api_->monotonic_now(&started) < 0) {
            record_first(clock_failure(runtime_api_), reactor_failure);
            cancel_all(media_failure);
            return;
        }
        struct timespec deadline;
        add_milliseconds(started, config_.stop_timeout_ms(), &deadline);

        std::vector<bool> stopped(pipelines_.size(), false);
        std::size_t remaining = pipelines_.size();
        while (remaining > 0U && media_failure->ok() && reactor_failure->ok()) {
            for (std::size_t reverse = pipelines_.size(); reverse > 0U;
                 --reverse) {
                const std::size_t index = reverse - 1U;
                if (stopped[index]) continue;
                const Status status = pipelines_[index].pipeline->stop();
                if (status.ok()) {
                    stopped[index] = true;
                    --remaining;
                    continue;
                }
                if (status.code() == StatusCode::kWouldBlock ||
                    status.code() == StatusCode::kNotFound) {
                    continue;
                }
                record_first(status, media_failure);
                record_first(observe_pipeline_failure(), observer_failure);
                break;
            }
            if (remaining == 0U || !media_failure->ok()) break;

            struct timespec now;
            if (runtime_api_->monotonic_now(&now) < 0) {
                record_first(clock_failure(runtime_api_), reactor_failure);
                break;
            }
            if (reached(now, deadline)) {
                record_first(Status(StatusCode::kTimeout,
                                    "Linux Runtime 停止排空超时"),
                             media_failure);
                record_first(observe_pipeline_failure(), observer_failure);
                break;
            }
        }

        if (!media_failure->ok() || !reactor_failure->ok()) {
            cancel_all(media_failure);
        }
    }

    void reactor_main_impl() {
        Status media_failure = Status::ok_status();
        Status reactor_failure = Status::ok_status();
        Status observer_failure = Status::ok_status();
        std::size_t attempted_count = 0U;

        for (std::size_t index = 0U; index < pipelines_.size(); ++index) {
            attempted_count = index + 1U;
            const Status status = pipelines_[index].pipeline->start();
            if (!status.ok()) {
                record_first(status, &media_failure);
                record_first(observe_pipeline_failure(), &observer_failure);
                fail_startup(media_failure, reactor_failure, observer_failure,
                             attempted_count, false);
                return;
            }
        }

        const Status initialized = loop_->initialize();
        if (!initialized.ok()) {
            record_first(initialized, &reactor_failure);
            fail_startup(media_failure, reactor_failure, observer_failure,
                         attempted_count, false);
            return;
        }

        for (std::size_t pipeline_index = 0U;
             pipeline_index < pipelines_.size(); ++pipeline_index) {
            PipelineRecord& record = pipelines_[pipeline_index];
            for (std::size_t source_index = 0U;
                 source_index < record.wait_sources.size(); ++source_index) {
                const Status registered = loop_->register_source(
                    record.pipeline, record.wait_sources[source_index]);
                if (!registered.ok()) {
                    if (registered.provider_id() == "linux_runtime") {
                        record_first(registered, &reactor_failure);
                    } else {
                        record_first(registered, &media_failure);
                        record_first(observe_pipeline_failure(),
                                     &observer_failure);
                    }
                    fail_startup(media_failure, reactor_failure,
                                 observer_failure, attempted_count, false);
                    return;
                }
            }
        }

        record_first(observe_running(true), &observer_failure);
        if (!observer_failure.ok()) {
            fail_startup(media_failure, reactor_failure, observer_failure,
                         attempted_count, true);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = PlatformRuntimeState::kRunning;
            condition_.notify_all();
        }

        while (media_failure.ok() && reactor_failure.ok() &&
               observer_failure.ok()) {
            Result<detail::LinuxEventLoopTurn> turn = loop_->wait_once();
            if (!turn.ok()) {
                record_first(turn.status(), &reactor_failure);
                break;
            }
            record_first(observe_poll(turn.value()), &observer_failure);
            if (stop_requested()) break;

            for (std::size_t index = 0U;
                 index < turn.value().ready_pipelines.size(); ++index) {
                record_first(observe_pipeline_turn(), &observer_failure);
                const Status ticked =
                    turn.value().ready_pipelines[index]->tick();
                if (!ticked.ok()) {
                    record_first(ticked, &media_failure);
                    record_first(observe_pipeline_failure(),
                                 &observer_failure);
                    break;
                }
            }
        }

        if (media_failure.ok() && reactor_failure.ok() &&
            observer_failure.ok() && stop_requested()) {
            mark_reactor_exiting();
            drain(&media_failure, &reactor_failure, &observer_failure);
        } else {
            mark_reactor_exiting();
            cancel_all(&media_failure);
        }

        record_first(loop_->close(), &reactor_failure);
        record_first(observe_running(false), &observer_failure);
        finish(media_failure, reactor_failure, observer_failure);
    }

    void reactor_main() noexcept {
        try {
            reactor_main_impl();
        } catch (const std::bad_alloc&) {
            mark_reactor_exiting();
            Status media_failure = allocation_failure();
            cancel_all(&media_failure);
            Status reactor_failure = loop_->close();
            Status observer_failure = observe_running(false);
            finish(media_failure, reactor_failure, observer_failure);
        } catch (...) {
            mark_reactor_exiting();
            Status media_failure = internal_failure();
            cancel_all(&media_failure);
            Status reactor_failure = loop_->close();
            Status observer_failure = observe_running(false);
            finish(media_failure, reactor_failure, observer_failure);
        }
    }

    LinuxPlatformRuntimeConfig config_;
    std::unique_ptr<detail::RuntimeObserver> owned_observer_;
    detail::RuntimeObserver* observer_;
    detail::LinuxRuntimeApi* runtime_api_;
    std::unique_ptr<detail::LinuxEventLoop> loop_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    PlatformRuntimeState state_;
    Status last_failure_;
    Status stop_result_;
    bool thread_exited_;
    bool stop_requested_;
    bool reactor_exiting_;
    std::vector<PipelineRecord> pipelines_;
    std::set<MediaPipeline*> registered_pipelines_;
    std::set<LinuxWaitSource*> registered_sources_;
    std::thread reactor_thread_;
};

Result<LinuxPlatformRuntimeConfig> LinuxPlatformRuntimeConfig::create(
    int reactor_count, int stop_timeout_ms) {
    if (reactor_count <= 0 || stop_timeout_ms <= 0) {
        return Result<LinuxPlatformRuntimeConfig>(
            Status(StatusCode::kInvalidArgument));
    }
    if (reactor_count != 1) {
        return Result<LinuxPlatformRuntimeConfig>(
            Status(StatusCode::kUnsupported));
    }

    try {
        return Result<LinuxPlatformRuntimeConfig>(
            LinuxPlatformRuntimeConfig(reactor_count, stop_timeout_ms));
    } catch (const std::bad_alloc&) {
        return Result<LinuxPlatformRuntimeConfig>(allocation_failure());
    } catch (...) {
        return Result<LinuxPlatformRuntimeConfig>(internal_failure());
    }
}

Result<std::unique_ptr<LinuxPlatformRuntime> > LinuxPlatformRuntime::create(
    const LinuxPlatformRuntimeConfig& config, MetricRegistry* metrics) {
    try {
        std::unique_ptr<LinuxPlatformRuntime> runtime(
            new LinuxPlatformRuntime(config, metrics));
        return Result<std::unique_ptr<LinuxPlatformRuntime> >(
            std::move(runtime));
    } catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<LinuxPlatformRuntime> >(
            allocation_failure());
    } catch (...) {
        return Result<std::unique_ptr<LinuxPlatformRuntime> >(
            internal_failure());
    }
}

LinuxPlatformRuntime::~LinuxPlatformRuntime() noexcept {
    try {
        impl_->stop();
    } catch (...) {
        impl_->join_noexcept();
    }
}

Status LinuxPlatformRuntime::register_pipeline(
    MediaPipeline* pipeline,
    const std::vector<LinuxWaitSource*>& wait_sources) {
    return impl_->register_pipeline(pipeline, wait_sources);
}

Status LinuxPlatformRuntime::start() {
    return impl_->start();
}

Status LinuxPlatformRuntime::stop() {
    return impl_->stop();
}

PlatformRuntimeState LinuxPlatformRuntime::state() const {
    return impl_->state();
}

Status LinuxPlatformRuntime::last_failure() const {
    return impl_->last_failure();
}

LinuxPlatformRuntime::LinuxPlatformRuntime(
    const LinuxPlatformRuntimeConfig& config, MetricRegistry* metrics)
    : impl_() {
    TestDependencies* dependencies = test_dependencies_slot();
    if (dependencies != NULL) {
        impl_.reset(new Impl(config, metrics, std::move(dependencies->api),
                             dependencies->observer));
    } else {
        impl_.reset(new Impl(config, metrics,
                             detail::create_linux_runtime_api(), NULL));
    }
}

namespace detail {

Result<std::unique_ptr<LinuxPlatformRuntime> >
LinuxPlatformRuntimeTestPeer::create(
    const LinuxPlatformRuntimeConfig& config,
    std::unique_ptr<LinuxRuntimeApi> api,
    RuntimeObserver* observer) {
    if (!api) {
        return Result<std::unique_ptr<LinuxPlatformRuntime> >(
            Status(StatusCode::kInvalidArgument));
    }
    TestDependencies dependencies(std::move(api), observer);
    ScopedTestDependencies scoped(&dependencies);
    return LinuxPlatformRuntime::create(config, NULL);
}

}  // namespace detail
}  // namespace eavp
