#include "eavp/platform/linux/platform_runtime.hpp"

#include <cerrno>
#include <condition_variable>
#include <ctime>
#include <limits>
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

const int kMaximumInterruptedSleepAttempts = 64;
const int kDrainBackoffMilliseconds = 1;

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

Status sleep_failure(detail::LinuxRuntimeApi* api) {
    return Status(StatusCode::kIoError, "Linux Runtime 停止排空等待失败",
                  "linux_runtime", "clock_nanosleep", api->last_error());
}

Status add_milliseconds(const struct timespec& start, int milliseconds,
                        struct timespec* deadline) {
    const long nanoseconds =
        start.tv_nsec + static_cast<long>(milliseconds % 1000) * 1000000L;
    const time_t carry = nanoseconds >= 1000000000L
                             ? static_cast<time_t>(1)
                             : static_cast<time_t>(0);
    const time_t seconds = static_cast<time_t>(milliseconds / 1000);
    const time_t maximum = std::numeric_limits<time_t>::max();
    if (start.tv_nsec < 0 || start.tv_nsec >= 1000000000L ||
        milliseconds < 0 || seconds > maximum - carry ||
        start.tv_sec > maximum - (seconds + carry)) {
        return Status(StatusCode::kResourceExhausted,
                      "Linux Runtime 停止期限超出 time_t 可表示范围");
    }

    deadline->tv_sec = start.tv_sec + seconds + carry;
    deadline->tv_nsec = nanoseconds -
                        static_cast<long>(carry) * 1000000000L;
    return Status::ok_status();
}

bool reached(const struct timespec& now, const struct timespec& deadline) {
    return now.tv_sec > deadline.tv_sec ||
           (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec);
}

bool earlier(const struct timespec& left, const struct timespec& right) {
    return left.tv_sec < right.tv_sec ||
           (left.tv_sec == right.tv_sec && left.tv_nsec < right.tv_nsec);
}

Status wait_before_retry(detail::LinuxRuntimeApi* api,
                         const struct timespec& now,
                         const struct timespec& deadline) {
    struct timespec retry_deadline = deadline;
    struct timespec candidate = {0, 0};
    const Status candidate_status = add_milliseconds(
        now, kDrainBackoffMilliseconds, &candidate);
    if (candidate_status.ok() && earlier(candidate, deadline)) {
        retry_deadline = candidate;
    }

    for (int attempt = 0; attempt < kMaximumInterruptedSleepAttempts;
         ++attempt) {
        if (api->monotonic_sleep_until(&retry_deadline) == 0) {
            return Status::ok_status();
        }
        if (api->last_error() != EINTR) return sleep_failure(api);
    }
    return sleep_failure(api);
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
                     detail::RuntimeObserver* runtime_observer,
                     detail::RuntimeTestHooks* runtime_hooks)
        : api(std::move(runtime_api)), observer(runtime_observer),
          hooks(runtime_hooks) {}

    std::unique_ptr<detail::LinuxRuntimeApi> api;
    detail::RuntimeObserver* observer;
    detail::RuntimeTestHooks* hooks;
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
         detail::RuntimeObserver* observer, detail::RuntimeTestHooks* hooks)
        : config_(config), owned_observer_(), observer_(observer), hooks_(hooks),
          runtime_api_(api.get()), loop_(new detail::LinuxEventLoop(std::move(api))),
          state_(PlatformRuntimeState::kCreated),
          last_failure_(StatusCode::kInvalidState),
          stop_result_(Status::ok_status()),
          startup_result_(StatusCode::kInvalidState),
          startup_completed_(false), thread_exited_(false),
          stop_requested_(false), reactor_exiting_(false), wake_in_flight_(0U),
          join_claimed_(false), join_completed_(false),
          pipelines_(), registered_pipelines_(), registered_sources_(),
          reactor_thread_() {
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
            wake_in_flight_ = 0U;
            join_claimed_ = false;
            join_completed_ = false;
            stop_result_ = Status::ok_status();
            startup_result_ = Status(StatusCode::kInvalidState);
            startup_completed_ = false;
            try {
                reactor_thread_ = std::thread(&Impl::reactor_main, this);
            } catch (const std::bad_alloc&) {
                last_failure_ = allocation_failure();
                stop_result_ = last_failure_;
                startup_result_ = last_failure_;
                startup_completed_ = true;
                state_ = PlatformRuntimeState::kError;
                thread_exited_ = true;
                condition_.notify_all();
                return last_failure_;
            } catch (...) {
                last_failure_ = internal_failure();
                stop_result_ = last_failure_;
                startup_result_ = last_failure_;
                startup_completed_ = true;
                state_ = PlatformRuntimeState::kError;
                thread_exited_ = true;
                condition_.notify_all();
                return last_failure_;
            }

            condition_.wait(lock, [this]() {
                return startup_completed_;
            });
            lock.unlock();
            invoke_hook_noexcept(
                [this]() { hooks_->on_startup_result_ready(); });
            lock.lock();
            const Status result = startup_result_;
            const bool started = result.ok();
            lock.unlock();
            if (!started) join_reactor();
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
            }
            if (state_ == PlatformRuntimeState::kStopped) {
                lock.unlock();
                join_reactor();
                lock.lock();
                return stop_result_;
            }
            if (state_ == PlatformRuntimeState::kError && thread_exited_) {
                const Status result = stop_result_;
                lock.unlock();
                join_reactor();
                return result;
            }
            if (reactor_exiting_) {
                lock.unlock();
                join_reactor();
                lock.lock();
                return stop_result_;
            }

            stop_requested_ = true;
            state_ = PlatformRuntimeState::kStopping;
            ++wake_in_flight_;
            lock.unlock();

            Status wake_status;
            try {
                notify_wake_claimed();
                wake_status = loop_->wake();
            } catch (...) {
                lock.lock();
                --wake_in_flight_;
                condition_.notify_all();
                lock.unlock();
                throw;
            }
            lock.lock();
            --wake_in_flight_;
            condition_.notify_all();
            if (!wake_status.ok() && stop_result_.ok()) {
                stop_result_ = wake_status;
            }
            lock.unlock();

            join_reactor();

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
        try {
            PlatformRuntimeState snapshot;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot = state_;
            }
            return hooks_ == NULL ? snapshot : hooks_->snapshot_state(snapshot);
        } catch (...) {
            return PlatformRuntimeState::kError;
        }
    }

    Status last_failure() const {
        try {
            Status snapshot;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot = last_failure_;
            }
            return hooks_ == NULL
                       ? snapshot
                       : hooks_->snapshot_last_failure(snapshot);
        } catch (const std::bad_alloc&) {
            return allocation_failure();
        } catch (...) {
            return internal_failure();
        }
    }

    void join_noexcept() noexcept {
        try {
            join_reactor();
        } catch (...) {
        }
    }

private:
    void join_reactor() {
        std::unique_lock<std::mutex> lock(mutex_);
        bool reported_as_waiter = false;
        for (;;) {
            if (join_completed_) return;
            if (!join_claimed_) {
                if (!reactor_thread_.joinable()) {
                    join_completed_ = true;
                    condition_.notify_all();
                    return;
                }
                join_claimed_ = true;
                break;
            }
            if (!reported_as_waiter) {
                lock.unlock();
                invoke_hook_noexcept([this]() { hooks_->on_join_waiter(); });
                lock.lock();
                reported_as_waiter = true;
                continue;
            }
            condition_.wait(lock, [this]() {
                return join_completed_ || !join_claimed_;
            });
        }

        lock.unlock();
        notify_join_owner();
        try {
            reactor_thread_.join();
        } catch (...) {
            lock.lock();
            join_claimed_ = false;
            if (!reactor_thread_.joinable()) join_completed_ = true;
            condition_.notify_all();
            throw;
        }
        lock.lock();
        join_claimed_ = false;
        join_completed_ = true;
        condition_.notify_all();
    }

    template <typename HookCall>
    void invoke_hook_noexcept(const HookCall& call) const noexcept {
        if (hooks_ == NULL) return;
        try {
            call();
        } catch (...) {
        }
    }

    void notify_wake_claimed() const noexcept {
        invoke_hook_noexcept([this]() { hooks_->on_wake_claimed(); });
    }

    void notify_join_owner() const noexcept {
        invoke_hook_noexcept([this]() { hooks_->on_join_owner_claimed(); });
    }

    void notify_thread_finishing() const noexcept {
        invoke_hook_noexcept(
            [this]() { hooks_->on_reactor_thread_finishing(); });
    }

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
        if (!startup_completed_) {
            startup_result_ = final_failure.ok()
                                  ? internal_failure()
                                  : final_failure;
            startup_completed_ = true;
        }
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
        std::unique_lock<std::mutex> lock(mutex_);
        reactor_exiting_ = true;
        lock.unlock();
        invoke_hook_noexcept(
            [this]() { hooks_->on_reactor_close_pending(); });
        lock.lock();
        if (wake_in_flight_ > 0U) {
            lock.unlock();
            invoke_hook_noexcept(
                [this]() { hooks_->on_reactor_waiting_for_wake(); });
            lock.lock();
            condition_.wait(lock, [this]() { return wake_in_flight_ == 0U; });
        }
    }

    void drain(Status* media_failure, Status* reactor_failure,
               Status* observer_failure) {
        struct timespec started;
        if (runtime_api_->monotonic_now(&started) < 0) {
            record_first(clock_failure(runtime_api_), reactor_failure);
            cancel_all(media_failure);
            return;
        }
        struct timespec deadline = {0, 0};
        const Status deadline_status = add_milliseconds(
            started, config_.stop_timeout_ms(), &deadline);
        if (!deadline_status.ok()) {
            record_first(deadline_status, reactor_failure);
            cancel_all(media_failure);
            return;
        }

        std::vector<bool> stopped(pipelines_.size(), false);
        std::size_t remaining = pipelines_.size();
        while (remaining > 0U && media_failure->ok() && reactor_failure->ok()) {
            bool made_progress = false;
            for (std::size_t reverse = pipelines_.size(); reverse > 0U;
                 --reverse) {
                const std::size_t index = reverse - 1U;
                if (stopped[index]) continue;
                const Status status = pipelines_[index].pipeline->stop();
                if (status.ok()) {
                    stopped[index] = true;
                    --remaining;
                    made_progress = true;
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
            if (!made_progress) {
                const Status wait_status = wait_before_retry(
                    runtime_api_, now, deadline);
                if (!wait_status.ok()) {
                    record_first(wait_status, reactor_failure);
                    break;
                }
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
            startup_result_ = Status::ok_status();
            startup_completed_ = true;
            condition_.notify_all();
        }

        while (media_failure.ok() && reactor_failure.ok() &&
               observer_failure.ok()) {
            Result<detail::LinuxEventLoopTurn> turn = loop_->wait_once();
            if (!turn.ok()) {
                if (loop_->wait_failure_origin() ==
                    detail::LinuxEventLoopWaitFailureOrigin::kWaitSource) {
                    record_first(turn.status(), &media_failure);
                    record_first(observe_pipeline_failure(),
                                 &observer_failure);
                } else {
                    record_first(turn.status(), &reactor_failure);
                }
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
        notify_thread_finishing();
    }

    LinuxPlatformRuntimeConfig config_;
    std::unique_ptr<detail::RuntimeObserver> owned_observer_;
    detail::RuntimeObserver* observer_;
    detail::RuntimeTestHooks* hooks_;
    detail::LinuxRuntimeApi* runtime_api_;
    std::unique_ptr<detail::LinuxEventLoop> loop_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    PlatformRuntimeState state_;
    Status last_failure_;
    Status stop_result_;
    Status startup_result_;
    bool startup_completed_;
    bool thread_exited_;
    bool stop_requested_;
    bool reactor_exiting_;
    std::size_t wake_in_flight_;
    bool join_claimed_;
    bool join_completed_;
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
    try {
        return impl_->register_pipeline(pipeline, wait_sources);
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    } catch (...) {
        return internal_failure();
    }
}

Status LinuxPlatformRuntime::start() {
    try {
        return impl_->start();
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    } catch (...) {
        return internal_failure();
    }
}

Status LinuxPlatformRuntime::stop() {
    try {
        return impl_->stop();
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    } catch (...) {
        return internal_failure();
    }
}

PlatformRuntimeState LinuxPlatformRuntime::state() const {
    try {
        return impl_->state();
    } catch (...) {
        return PlatformRuntimeState::kError;
    }
}

Status LinuxPlatformRuntime::last_failure() const {
    try {
        return impl_->last_failure();
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    } catch (...) {
        return internal_failure();
    }
}

LinuxPlatformRuntime::LinuxPlatformRuntime(
    const LinuxPlatformRuntimeConfig& config, MetricRegistry* metrics)
    : impl_() {
    TestDependencies* dependencies = test_dependencies_slot();
    if (dependencies != NULL) {
        impl_.reset(new Impl(config, metrics, std::move(dependencies->api),
                             dependencies->observer, dependencies->hooks));
    } else {
        impl_.reset(new Impl(config, metrics,
                             detail::create_linux_runtime_api(), NULL, NULL));
    }
}

namespace detail {

Result<std::unique_ptr<LinuxPlatformRuntime> >
LinuxPlatformRuntimeTestPeer::create(
    const LinuxPlatformRuntimeConfig& config,
    std::unique_ptr<LinuxRuntimeApi> api,
    RuntimeObserver* observer,
    RuntimeTestHooks* hooks) {
    if (!api) {
        return Result<std::unique_ptr<LinuxPlatformRuntime> >(
            Status(StatusCode::kInvalidArgument));
    }
    TestDependencies dependencies(std::move(api), observer, hooks);
    ScopedTestDependencies scoped(&dependencies);
    return LinuxPlatformRuntime::create(config, NULL);
}

}  // namespace detail
}  // namespace eavp
