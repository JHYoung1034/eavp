#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/epoll.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "eavp/management/metrics.hpp"
#include "eavp/media/node.hpp"
#include "eavp/media/pipeline.hpp"
#include "eavp/platform/linux/platform_runtime.hpp"
#include "eavp/platform/linux/wait_source.hpp"
#include "../../src/platform/linux/platform_runtime_internal.hpp"
#include "../support/runtime_test_utils.hpp"

namespace {

class FakeWaitSource : public eavp::LinuxWaitSource {
public:
    explicit FakeWaitSource(const std::vector<struct pollfd>& descriptors)
        : descriptors_(descriptors), evaluated_descriptors_() {}

    eavp::Result<std::vector<struct pollfd> > poll_descriptors() override {
        return eavp::Result<std::vector<struct pollfd> >(descriptors_);
    }

    eavp::Result<bool> evaluate_poll_events(
        const std::vector<struct pollfd>& descriptors) override {
        evaluated_descriptors_ = descriptors;
        return eavp::Result<bool>(true);
    }

    const std::vector<struct pollfd>& evaluated_descriptors() const {
        return evaluated_descriptors_;
    }

private:
    std::vector<struct pollfd> descriptors_;
    std::vector<struct pollfd> evaluated_descriptors_;
};

using eavp_test::FakeLinuxRuntimeApi;
using eavp_test::RuntimeFixture;
using eavp_test::readable_fd;

class ScopedTestAlarm {
public:
    ScopedTestAlarm() { ::alarm(5U); }
    ~ScopedTestAlarm() { ::alarm(0U); }

private:
    ScopedTestAlarm(const ScopedTestAlarm&);
    ScopedTestAlarm& operator=(const ScopedTestAlarm&);
};

class PipeWaitSource : public eavp::LinuxWaitSource {
public:
    PipeWaitSource()
        : read_fd_(-1), write_fd_(-1), evaluate_failure_() {
        int descriptors[2] = {-1, -1};
        if (::pipe(descriptors) != 0) {
            throw std::runtime_error("pipe");
        }
        read_fd_ = descriptors[0];
        write_fd_ = descriptors[1];
        if (::fcntl(read_fd_, F_SETFL, O_NONBLOCK) != 0 ||
            ::fcntl(write_fd_, F_SETFL, O_NONBLOCK) != 0) {
            ::close(read_fd_);
            ::close(write_fd_);
            throw std::runtime_error("fcntl");
        }
    }

    ~PipeWaitSource() {
        if (write_fd_ >= 0) ::close(write_fd_);
        if (read_fd_ >= 0) ::close(read_fd_);
    }

    eavp::Result<std::vector<struct pollfd> > poll_descriptors() override {
        const struct pollfd descriptor = {
            read_fd_, static_cast<short>(POLLIN), 0};
        return eavp::Result<std::vector<struct pollfd> >(
            std::vector<struct pollfd>(1U, descriptor));
    }

    eavp::Result<bool> evaluate_poll_events(
        const std::vector<struct pollfd>& descriptors) override {
        if (!evaluate_failure_.ok()) {
            return eavp::Result<bool>(evaluate_failure_);
        }
        if (descriptors.size() != 1U) {
            return eavp::Result<bool>(
                eavp::Status(eavp::StatusCode::kInvalidArgument));
        }
        return eavp::Result<bool>(
            (descriptors[0].revents & static_cast<short>(POLLIN | POLLERR |
                                                        POLLHUP)) != 0);
    }

    bool signal() {
        const char value = 'x';
        return ::write(write_fd_, &value, sizeof(value)) == sizeof(value);
    }

    int read_fd() const { return read_fd_; }
    void set_evaluate_failure(const eavp::Status& failure) {
        evaluate_failure_ = failure;
    }

private:
    int read_fd_;
    int write_fd_;
    eavp::Status evaluate_failure_;
};

class RecordingNode : public eavp::MediaNode {
public:
    RecordingNode(const std::string& id, int ready_fd,
                  const eavp::Status& start_status = eavp::Status::ok_status(),
                  const eavp::Status& tick_status = eavp::Status::ok_status(),
                  bool drain_forever = false,
                  std::vector<std::string>* external_log = NULL)
        : eavp::MediaNode(id), ready_fd_(ready_fd),
          start_status_(start_status), tick_status_(tick_status),
          drain_forever_(drain_forever), external_log_(external_log),
          additional_ready_fd_(-1), reset_status_(), block_tick_exit_(false),
          tick_exit_allowed_(false),
          tick_count_(0), stop_count_(0), reset_count_(0),
          tick_in_progress_(false), concurrent_tick_observed_(false),
          calls_() {}

    bool wait_for_ticks(int expected) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2),
                                   [this, expected]() {
                                       return tick_count_ >= expected;
                                   });
    }

    bool all_calls_share_one_thread() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (calls_.empty()) return false;
        for (std::size_t index = 1U; index < calls_.size(); ++index) {
            if (calls_[index] != calls_[0]) return false;
        }
        return true;
    }

    std::thread::id execution_thread() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return calls_.empty() ? std::thread::id() : calls_[0];
    }

    bool concurrent_tick_observed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return concurrent_tick_observed_;
    }

    int tick_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tick_count_;
    }

    int stop_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stop_count_;
    }

    int reset_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return reset_count_;
    }

    void set_additional_ready_fd(int fd) { additional_ready_fd_ = fd; }
    void set_reset_status(const eavp::Status& status) { reset_status_ = status; }

    void block_tick_exit() {
        std::lock_guard<std::mutex> lock(mutex_);
        block_tick_exit_ = true;
        tick_exit_allowed_ = false;
    }

    void allow_tick_exit() {
        std::lock_guard<std::mutex> lock(mutex_);
        tick_exit_allowed_ = true;
        condition_.notify_all();
    }

protected:
    eavp::Status on_prepare() override {
        record("prepare");
        return eavp::Status::ok_status();
    }

    eavp::Status on_start() override {
        record("start");
        return start_status_;
    }

    eavp::Status on_stop() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stop_count_;
        }
        record("stop");
        return drain_forever_
                   ? eavp::Status(eavp::StatusCode::kWouldBlock)
                   : eavp::Status::ok_status();
    }

    eavp::Status on_reset() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++reset_count_;
        }
        record("reset");
        return reset_status_;
    }

    eavp::Status on_tick() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (tick_in_progress_) concurrent_tick_observed_ = true;
            tick_in_progress_ = true;
            ++tick_count_;
            calls_.push_back(std::this_thread::get_id());
            if (external_log_ != NULL) external_log_->push_back(id() + ".tick");
            condition_.notify_all();
        }
        char value = 0;
        if (ready_fd_ >= 0) {
            const ssize_t ignored = ::read(ready_fd_, &value, sizeof(value));
            (void)ignored;
        }
        if (additional_ready_fd_ >= 0) {
            const ssize_t ignored =
                ::read(additional_ready_fd_, &value, sizeof(value));
            (void)ignored;
        }
        {
            std::unique_lock<std::mutex> lock(mutex_);
            while (block_tick_exit_ && !tick_exit_allowed_) {
                condition_.wait(lock);
            }
            tick_in_progress_ = false;
            condition_.notify_all();
        }
        return tick_status_;
    }

private:
    void record(const std::string& operation) {
        std::lock_guard<std::mutex> lock(mutex_);
        calls_.push_back(std::this_thread::get_id());
        if (external_log_ != NULL) external_log_->push_back(id() + "." + operation);
        condition_.notify_all();
    }

    int ready_fd_;
    eavp::Status start_status_;
    eavp::Status tick_status_;
    bool drain_forever_;
    std::vector<std::string>* external_log_;
    int additional_ready_fd_;
    eavp::Status reset_status_;
    bool block_tick_exit_;
    bool tick_exit_allowed_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    int tick_count_;
    int stop_count_;
    int reset_count_;
    bool tick_in_progress_;
    bool concurrent_tick_observed_;
    std::vector<std::thread::id> calls_;
};

struct RuntimePipelineHarness {
    RuntimePipelineHarness(
        const std::string& id,
        const eavp::Status& start_status = eavp::Status::ok_status(),
        const eavp::Status& tick_status = eavp::Status::ok_status(),
        bool drain_forever = false,
        std::vector<std::string>* external_log = NULL)
        : deadline(), source(), node(NULL), pipeline(id) {
        std::unique_ptr<RecordingNode> owned_node(new RecordingNode(
            id + "-node", source.read_fd(), start_status, tick_status,
            drain_forever, external_log));
        node = owned_node.get();
        const eavp::Status added = pipeline.add_node(std::move(owned_node));
        if (!added.ok()) throw std::runtime_error("add_node");
    }

    std::vector<eavp::LinuxWaitSource*> wait_sources() {
        return std::vector<eavp::LinuxWaitSource*>(1U, &source);
    }

    ScopedTestAlarm deadline;
    PipeWaitSource source;
    RecordingNode* node;
    eavp::MediaPipeline pipeline;
};

std::unique_ptr<eavp::LinuxPlatformRuntime> create_runtime(
    int stop_timeout_ms = 100, eavp::MetricRegistry* metrics = NULL) {
    const eavp::LinuxPlatformRuntimeConfig config =
        eavp::LinuxPlatformRuntimeConfig::create(1, stop_timeout_ms).take_value();
    eavp::Result<std::unique_ptr<eavp::LinuxPlatformRuntime> > created =
        eavp::LinuxPlatformRuntime::create(config, metrics);
    if (!created.ok()) throw std::runtime_error("runtime create");
    return created.take_value();
}

class FakeRuntimeObserver : public eavp::detail::RuntimeObserver {
public:
    FakeRuntimeObserver()
        : poll_status_(), turn_status_(), failure_status_(),
          running_status_(), throw_on_turn_(false), poll_count_(0),
          turn_count_(0), failure_count_(0), running_true_count_(0),
          running_false_count_(0) {}

    eavp::Status on_poll(std::uint64_t, std::uint64_t) override {
        ++poll_count_;
        return poll_status_;
    }

    eavp::Status on_pipeline_turn() override {
        ++turn_count_;
        if (throw_on_turn_) throw std::runtime_error("observer turn");
        return turn_status_;
    }

    eavp::Status on_pipeline_failure() override {
        ++failure_count_;
        return failure_status_;
    }

    eavp::Status on_reactor_running(bool running) override {
        if (running) {
            ++running_true_count_;
            return running_status_;
        }
        ++running_false_count_;
        return eavp::Status::ok_status();
    }

    eavp::Status poll_status_;
    eavp::Status turn_status_;
    eavp::Status failure_status_;
    eavp::Status running_status_;
    bool throw_on_turn_;
    int poll_count_;
    int turn_count_;
    int failure_count_;
    int running_true_count_;
    int running_false_count_;
};

class RuntimeConcurrencyProbe : public eavp::detail::RuntimeTestHooks {
public:
    RuntimeConcurrencyProbe()
        : block_wake_claim_(false), block_close_pending_(false),
          block_write_(false), block_thread_finish_(false),
          block_join_owner_(false), throw_bad_alloc_on_state_(false),
          throw_unknown_on_state_(false),
          throw_bad_alloc_on_last_failure_(false),
          throw_unknown_on_last_failure_(false), wake_claimed_(false),
          allow_wake_claim_(false), close_pending_(false),
          allow_close_pending_(false), reactor_waiting_for_wake_(false),
          write_entered_(false), write_in_progress_(false),
          allow_write_(false), close_call_count_(0),
          close_while_write_(false), thread_finishing_(false),
          allow_thread_finish_(false), join_owner_count_(0),
          join_waiter_count_(0), allow_first_join_owner_(false),
          allow_later_join_owners_(false) {}

    void on_wake_claimed() override {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_claimed_ = true;
        condition_.notify_all();
        while (block_wake_claim_ && !allow_wake_claim_) {
            condition_.wait(lock);
        }
    }

    void on_reactor_close_pending() override {
        std::unique_lock<std::mutex> lock(mutex_);
        close_pending_ = true;
        condition_.notify_all();
        while (block_close_pending_ && !allow_close_pending_) {
            condition_.wait(lock);
        }
    }

    void on_reactor_waiting_for_wake() override {
        std::lock_guard<std::mutex> lock(mutex_);
        reactor_waiting_for_wake_ = true;
        condition_.notify_all();
    }

    void on_join_owner_claimed() override {
        std::unique_lock<std::mutex> lock(mutex_);
        const int ordinal = ++join_owner_count_;
        condition_.notify_all();
        while (block_join_owner_ &&
               !((ordinal == 1 && allow_first_join_owner_) ||
                 (ordinal > 1 && allow_later_join_owners_))) {
            condition_.wait(lock);
        }
    }

    void on_join_waiter() override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++join_waiter_count_;
        condition_.notify_all();
    }

    void on_reactor_thread_finishing() override {
        std::unique_lock<std::mutex> lock(mutex_);
        thread_finishing_ = true;
        condition_.notify_all();
        while (block_thread_finish_ && !allow_thread_finish_) {
            condition_.wait(lock);
        }
    }

    eavp::PlatformRuntimeState snapshot_state(
        eavp::PlatformRuntimeState state) override {
        if (throw_bad_alloc_on_state_) throw std::bad_alloc();
        if (throw_unknown_on_state_) throw std::runtime_error("state snapshot");
        return state;
    }

    eavp::Status snapshot_last_failure(
        const eavp::Status& failure) override {
        if (throw_bad_alloc_on_last_failure_) throw std::bad_alloc();
        if (throw_unknown_on_last_failure_) {
            throw std::runtime_error("failure snapshot");
        }
        return failure;
    }

    bool wait_for_wake_claimed() { return wait_for(&wake_claimed_); }
    bool wait_for_close_pending() { return wait_for(&close_pending_); }
    bool wait_for_write_entered() { return wait_for(&write_entered_); }
    bool wait_for_thread_finishing() { return wait_for(&thread_finishing_); }

    bool wait_for_close_or_wake_wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2), [this]() {
            return close_call_count_ > 0 || reactor_waiting_for_wake_;
        });
    }

    bool wait_for_join_participants(int expected) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2),
                                   [this, expected]() {
                                       return join_owner_count_ +
                                                  join_waiter_count_ >=
                                              expected;
                                   });
    }

    void allow_wake_claim() {
        std::lock_guard<std::mutex> lock(mutex_);
        allow_wake_claim_ = true;
        condition_.notify_all();
    }

    void allow_close_pending() {
        std::lock_guard<std::mutex> lock(mutex_);
        allow_close_pending_ = true;
        condition_.notify_all();
    }

    void allow_write() {
        std::lock_guard<std::mutex> lock(mutex_);
        allow_write_ = true;
        condition_.notify_all();
    }

    void allow_thread_and_first_join_owner() {
        std::lock_guard<std::mutex> lock(mutex_);
        allow_thread_finish_ = true;
        allow_first_join_owner_ = true;
        condition_.notify_all();
    }

    void allow_later_join_owners() {
        std::lock_guard<std::mutex> lock(mutex_);
        allow_later_join_owners_ = true;
        condition_.notify_all();
    }

    void allow_everything() {
        std::lock_guard<std::mutex> lock(mutex_);
        allow_wake_claim_ = true;
        allow_close_pending_ = true;
        allow_write_ = true;
        allow_thread_finish_ = true;
        allow_first_join_owner_ = true;
        allow_later_join_owners_ = true;
        condition_.notify_all();
    }

    void before_write() {
        std::unique_lock<std::mutex> lock(mutex_);
        write_entered_ = true;
        write_in_progress_ = true;
        condition_.notify_all();
        while (block_write_ && !allow_write_) condition_.wait(lock);
    }

    void after_write() {
        std::lock_guard<std::mutex> lock(mutex_);
        write_in_progress_ = false;
        condition_.notify_all();
    }

    void on_close() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++close_call_count_;
        if (write_in_progress_) close_while_write_ = true;
        condition_.notify_all();
    }

    int join_owner_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return join_owner_count_;
    }

    int join_waiter_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return join_waiter_count_;
    }

    bool reactor_waiting_for_wake() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return reactor_waiting_for_wake_;
    }

    bool close_while_write() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return close_while_write_;
    }

    bool block_wake_claim_;
    bool block_close_pending_;
    bool block_write_;
    bool block_thread_finish_;
    bool block_join_owner_;
    bool throw_bad_alloc_on_state_;
    bool throw_unknown_on_state_;
    bool throw_bad_alloc_on_last_failure_;
    bool throw_unknown_on_last_failure_;

private:
    bool wait_for(bool* value) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2),
                                   [value]() { return *value; });
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool wake_claimed_;
    bool allow_wake_claim_;
    bool close_pending_;
    bool allow_close_pending_;
    bool reactor_waiting_for_wake_;
    bool write_entered_;
    bool write_in_progress_;
    bool allow_write_;
    int close_call_count_;
    bool close_while_write_;
    bool thread_finishing_;
    bool allow_thread_finish_;
    int join_owner_count_;
    int join_waiter_count_;
    bool allow_first_join_owner_;
    bool allow_later_join_owners_;
};

class StartupResultProbe : public eavp::detail::RuntimeTestHooks {
public:
    StartupResultProbe()
        : entered_(false), released_(false) {}

    void on_startup_result_ready() override {
        std::unique_lock<std::mutex> lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        while (!released_) condition_.wait(lock);
    }

    bool wait_until_entered() {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2), [this]() {
            return entered_;
        });
    }

    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_;
    bool released_;
};

class InstrumentedLinuxRuntimeApi : public eavp::detail::LinuxRuntimeApi {
public:
    explicit InstrumentedLinuxRuntimeApi(RuntimeConcurrencyProbe* probe = NULL)
        : delegate_(eavp::detail::create_linux_runtime_api()), probe_(probe),
          fixed_clock_(false), fixed_time_() {}

    int epoll_create() override { return delegate_->epoll_create(); }
    int epoll_add(int epoll_fd, int fd, std::uint32_t events,
                  std::uint64_t token) override {
        return delegate_->epoll_add(epoll_fd, fd, events, token);
    }
    int epoll_remove(int epoll_fd, int fd) override {
        return delegate_->epoll_remove(epoll_fd, fd);
    }
    int epoll_wait_events(int epoll_fd, struct epoll_event* events,
                          int capacity, int timeout_ms) override {
        return delegate_->epoll_wait_events(epoll_fd, events, capacity,
                                            timeout_ms);
    }
    int create_event_fd() override { return delegate_->create_event_fd(); }
    int read_event_fd(int fd, std::uint64_t* value) override {
        return delegate_->read_event_fd(fd, value);
    }
    int write_event_fd(int fd, std::uint64_t value) override {
        if (probe_ != NULL) probe_->before_write();
        const int result = delegate_->write_event_fd(fd, value);
        if (probe_ != NULL) probe_->after_write();
        return result;
    }
    int close_fd(int fd) override {
        if (probe_ != NULL) probe_->on_close();
        return delegate_->close_fd(fd);
    }
    int monotonic_now(struct timespec* value) override {
        if (fixed_clock_) {
            *value = fixed_time_;
            return 0;
        }
        return delegate_->monotonic_now(value);
    }
    int monotonic_sleep_until(const struct timespec* deadline) override {
        return delegate_->monotonic_sleep_until(deadline);
    }
    int last_error() const override { return delegate_->last_error(); }

    void set_fixed_time(const struct timespec& value) {
        fixed_clock_ = true;
        fixed_time_ = value;
    }

private:
    std::unique_ptr<eavp::detail::LinuxRuntimeApi> delegate_;
    RuntimeConcurrencyProbe* probe_;
    bool fixed_clock_;
    struct timespec fixed_time_;
};

class ControlledWaitFailureApi : public FakeLinuxRuntimeApi {
public:
    enum FailureMode {
        kBadAlloc,
        kInternal,
    };

    ControlledWaitFailureApi(FailureMode mode, bool block_wait)
        : FakeLinuxRuntimeApi(), mode_(mode), block_wait_(block_wait),
          wait_entered_(false), wait_allowed_(!block_wait) {}

    int epoll_wait_events(int, struct epoll_event*, int, int) override {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wait_entered_ = true;
            condition_.notify_all();
            while (block_wait_ && !wait_allowed_) condition_.wait(lock);
        }
        if (mode_ == kBadAlloc) throw std::bad_alloc();
        throw std::runtime_error("epoll_wait");
    }

    bool wait_for_wait_entered() {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2), [this]() {
            return wait_entered_;
        });
    }

    void allow_wait() {
        std::lock_guard<std::mutex> lock(mutex_);
        wait_allowed_ = true;
        condition_.notify_all();
    }

private:
    FailureMode mode_;
    bool block_wait_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool wait_entered_;
    bool wait_allowed_;
};

std::unique_ptr<eavp::LinuxPlatformRuntime> create_injected_runtime(
    std::unique_ptr<eavp::detail::LinuxRuntimeApi> api,
    eavp::detail::RuntimeObserver* observer,
    eavp::detail::RuntimeTestHooks* hooks,
    int stop_timeout_ms = 100) {
    const eavp::LinuxPlatformRuntimeConfig config =
        eavp::LinuxPlatformRuntimeConfig::create(
            1, stop_timeout_ms).take_value();
    eavp::Result<std::unique_ptr<eavp::LinuxPlatformRuntime> > created =
        eavp::detail::LinuxPlatformRuntimeTestPeer::create(
            config, std::move(api), observer, hooks);
    if (!created.ok()) throw std::runtime_error("injected runtime create");
    return created.take_value();
}

std::unique_ptr<eavp::LinuxPlatformRuntime> create_test_runtime(
    eavp::detail::RuntimeObserver* observer, int stop_timeout_ms = 100) {
    return create_injected_runtime(
        eavp::detail::create_linux_runtime_api(), observer, NULL,
        stop_timeout_ms);
}

TEST(LinuxPlatformRuntimeConfigTest, AcceptsSingleReactorAndPositiveStopTimeout) {
    eavp::Result<eavp::LinuxPlatformRuntimeConfig> result =
        eavp::LinuxPlatformRuntimeConfig::create(1, 2000);

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(1, result.value().reactor_count());
    EXPECT_EQ(2000, result.value().stop_timeout_ms());
}

TEST(LinuxPlatformRuntimeConfigTest, RejectsInvalidOrUnsupportedValues) {
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::LinuxPlatformRuntimeConfig::create(0, 2000).status().code());
    EXPECT_EQ(eavp::StatusCode::kUnsupported,
              eavp::LinuxPlatformRuntimeConfig::create(2, 2000).status().code());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::LinuxPlatformRuntimeConfig::create(1, 0).status().code());
}

TEST(LinuxWaitSourceTest, PreservesTheCompletePollDescriptorArray) {
    const std::vector<struct pollfd> descriptors = {
        {17, static_cast<short>(POLLIN), 0},
        {23, static_cast<short>(POLLOUT | POLLPRI), 0},
    };
    FakeWaitSource source(descriptors);

    eavp::Result<std::vector<struct pollfd> > result = source.poll_descriptors();

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(2U, result.value().size());
    EXPECT_EQ(17, result.value()[0].fd);
    EXPECT_EQ(POLLIN, result.value()[0].events);
    EXPECT_EQ(23, result.value()[1].fd);
    EXPECT_EQ(static_cast<short>(POLLOUT | POLLPRI), result.value()[1].events);
    result.value()[0].revents = POLLIN;
    result.value()[1].revents = POLLPRI;
    ASSERT_TRUE(source.evaluate_poll_events(result.value()).ok());
    EXPECT_EQ(POLLIN, source.evaluated_descriptors()[0].revents);
    EXPECT_EQ(POLLPRI, source.evaluated_descriptors()[1].revents);
}

TEST(LinuxPlatformRuntimeTest, RejectsInvalidAndDuplicatePipelineRegistration) {
    RuntimePipelineHarness harness("registered");
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime = create_runtime();

    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              runtime->register_pipeline(
                  NULL, std::vector<eavp::LinuxWaitSource*>()).code());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              runtime->register_pipeline(
                  &harness.pipeline,
                  std::vector<eavp::LinuxWaitSource*>()).code());
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());
    EXPECT_EQ(eavp::StatusCode::kAlreadyExists,
              runtime->register_pipeline(
                  &harness.pipeline, harness.wait_sources()).code());
}

TEST(LinuxPlatformRuntimeTest, RunsAllPipelineOperationsOnOneReactorThread) {
    RuntimePipelineHarness harness("affinity");
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime = create_runtime();
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());

    ASSERT_TRUE(runtime->start().ok());
    ASSERT_TRUE(harness.source.signal());
    ASSERT_TRUE(harness.node->wait_for_ticks(1));
    ASSERT_TRUE(runtime->stop().ok());

    EXPECT_TRUE(harness.node->all_calls_share_one_thread());
    EXPECT_NE(std::this_thread::get_id(), harness.node->execution_thread());
    EXPECT_FALSE(harness.node->concurrent_tick_observed());
    EXPECT_EQ(1, harness.node->tick_count());
    EXPECT_EQ(1, harness.node->stop_count());
    EXPECT_EQ(1, harness.node->reset_count());
}

TEST(LinuxPlatformRuntimeTest, RollsBackStartedPipelinesInReverseOrder) {
    std::vector<std::string> calls;
    RuntimePipelineHarness first("first", eavp::Status::ok_status(),
                                 eavp::Status::ok_status(), false, &calls);
    RuntimePipelineHarness second(
        "second",
        eavp::Status(eavp::StatusCode::kDeviceLost, "second start failed"),
        eavp::Status::ok_status(), false, &calls);
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime = create_runtime();
    ASSERT_TRUE(runtime->register_pipeline(
        &first.pipeline, first.wait_sources()).ok());
    ASSERT_TRUE(runtime->register_pipeline(
        &second.pipeline, second.wait_sources()).ok());

    const eavp::Status started = runtime->start();

    EXPECT_EQ(eavp::StatusCode::kDeviceLost, started.code());
    EXPECT_EQ(eavp::PlatformRuntimeState::kError, runtime->state());
    EXPECT_EQ(eavp::StatusCode::kDeviceLost,
              runtime->last_failure().code());
    ASSERT_GE(calls.size(), 7U);
    EXPECT_EQ("second-node.reset", calls[calls.size() - 2U]);
    EXPECT_EQ("first-node.reset", calls[calls.size() - 1U]);
    EXPECT_TRUE(first.node->all_calls_share_one_thread());
    EXPECT_TRUE(second.node->all_calls_share_one_thread());
    EXPECT_EQ(first.node->execution_thread(), second.node->execution_thread());
    EXPECT_NE(std::this_thread::get_id(), first.node->execution_thread());
}

TEST(LinuxPlatformRuntimeTest, JoinsTheReactorBeforeStartFailureReturns) {
    RuntimePipelineHarness harness(
        "start-failure",
        eavp::Status(eavp::StatusCode::kDeviceLost, "camera disappeared"));
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime = create_runtime();
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());

    EXPECT_EQ(eavp::StatusCode::kDeviceLost, runtime->start().code());
    const int resets_after_return = harness.node->reset_count();
    EXPECT_GE(resets_after_return, 1);
    EXPECT_EQ(resets_after_return, harness.node->reset_count());
    EXPECT_EQ(eavp::StatusCode::kDeviceLost, runtime->stop().code());
}

TEST(LinuxPlatformRuntimeTest, StartFailureReturnsTheLatchedEnrichedStatus) {
    const eavp::Status failure(
        eavp::StatusCode::kDeviceLost, "camera disappeared",
        "camera", "start", ENODEV);
    RuntimePipelineHarness harness("enriched-start-failure", failure);
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime = create_runtime();
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());

    const eavp::Status started = runtime->start();

    EXPECT_EQ(eavp::StatusCode::kDeviceLost, started.code());
    EXPECT_EQ("camera disappeared", started.message());
    EXPECT_EQ("camera", started.provider_id());
    EXPECT_EQ("start", started.operation());
    EXPECT_EQ(ENODEV, started.native_code());
    EXPECT_EQ(started.code(), runtime->stop().code());
}

TEST(LinuxPlatformRuntimeTest, StopWakesAReactorBlockedInEpoll) {
    RuntimePipelineHarness harness("blocked-stop");
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime = create_runtime();
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());
    ASSERT_TRUE(runtime->start().ok());

    const std::chrono::steady_clock::time_point before =
        std::chrono::steady_clock::now();
    const eavp::Status stopped = runtime->stop();
    const std::chrono::milliseconds elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - before);

    EXPECT_TRUE(stopped.ok());
    EXPECT_LT(elapsed.count(), 1000);
    EXPECT_EQ(eavp::PlatformRuntimeState::kStopped, runtime->state());
}

TEST(LinuxPlatformRuntimeTest, StartAndStopAreIdempotent) {
    RuntimePipelineHarness harness("idempotent");
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime = create_runtime();
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());

    ASSERT_TRUE(runtime->start().ok());
    EXPECT_TRUE(runtime->start().ok());
    ASSERT_TRUE(runtime->stop().ok());
    EXPECT_TRUE(runtime->stop().ok());
    EXPECT_EQ(1, harness.node->stop_count());
    EXPECT_EQ(1, harness.node->reset_count());
}

TEST(LinuxPlatformRuntimeTest, PreservesFatalPipelineFailureAndStopsScheduling) {
    const eavp::Status fatal(
        eavp::StatusCode::kDeviceLost, "capture lost", "camera", "tick", 19);
    RuntimePipelineHarness harness(
        "fatal", eavp::Status::ok_status(), fatal);
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime = create_runtime();
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());
    ASSERT_TRUE(runtime->start().ok());

    ASSERT_TRUE(harness.source.signal());
    ASSERT_TRUE(harness.node->wait_for_ticks(1));
    const eavp::Status stopped = runtime->stop();

    EXPECT_EQ(eavp::StatusCode::kDeviceLost, stopped.code());
    EXPECT_EQ(eavp::PlatformRuntimeState::kError, runtime->state());
    const eavp::Status observed = runtime->last_failure();
    EXPECT_EQ(eavp::StatusCode::kDeviceLost, observed.code());
    EXPECT_EQ("camera", observed.provider_id());
    EXPECT_EQ("tick", observed.operation());
    EXPECT_EQ(19, observed.native_code());
    EXPECT_EQ(1, harness.node->tick_count());
    EXPECT_GE(harness.node->reset_count(), 1);
}

TEST(LinuxPlatformRuntimeTest, CancelsDrainAndReturnsTimeoutAtTheDeadline) {
    RuntimePipelineHarness harness(
        "timeout", eavp::Status::ok_status(),
        eavp::Status::ok_status(), true);
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime = create_runtime(1);
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());
    ASSERT_TRUE(runtime->start().ok());

    const eavp::Status stopped = runtime->stop();

    EXPECT_EQ(eavp::StatusCode::kTimeout, stopped.code());
    EXPECT_EQ(eavp::PlatformRuntimeState::kError, runtime->state());
    EXPECT_EQ(eavp::StatusCode::kTimeout,
              runtime->last_failure().code());
    EXPECT_GT(harness.node->stop_count(), 0);
    EXPECT_GE(harness.node->reset_count(), 1);
    EXPECT_EQ(stopped.code(), runtime->stop().code());
}

TEST(LinuxPlatformRuntimeTest,
     BacksOffWhenAnEntireDrainRoundMakesNoProgress) {
    RuntimePipelineHarness harness(
        "bounded-drain", eavp::Status::ok_status(),
        eavp::Status::ok_status(), true);
    FakeLinuxRuntimeApi* raw_api = new FakeLinuxRuntimeApi();
    raw_api->set_monotonic_step_ns(1000L);
    std::unique_ptr<eavp::detail::LinuxRuntimeApi> api(raw_api);
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime =
        create_injected_runtime(std::move(api), NULL, NULL, 1);
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());
    ASSERT_TRUE(runtime->start().ok());

    const eavp::Status stopped = runtime->stop();

    EXPECT_EQ(eavp::StatusCode::kTimeout, stopped.code());
    EXPECT_LE(harness.node->stop_count(), 2);
    EXPECT_EQ(1, raw_api->monotonic_sleep_count);
    ASSERT_EQ(1U, raw_api->sleep_deadlines.size());
    EXPECT_EQ(0, raw_api->sleep_deadlines[0].tv_sec);
    EXPECT_EQ(1000000L, raw_api->sleep_deadlines[0].tv_nsec);
}

TEST(LinuxPlatformRuntimeTest,
     DoesNotBackOffUntilAWholeDrainRoundMakesNoProgress) {
    RuntimePipelineHarness blocked(
        "blocked-progress", eavp::Status::ok_status(),
        eavp::Status::ok_status(), true);
    RuntimePipelineHarness completed("completed-progress");
    FakeLinuxRuntimeApi* raw_api = new FakeLinuxRuntimeApi();
    raw_api->set_monotonic_step_ns(1000L);
    int blocked_calls_at_first_sleep = 0;
    raw_api->sleep_callback = [&]() {
        if (blocked_calls_at_first_sleep == 0) {
            blocked_calls_at_first_sleep = blocked.node->stop_count();
        }
    };
    std::unique_ptr<eavp::detail::LinuxRuntimeApi> api(raw_api);
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime =
        create_injected_runtime(std::move(api), NULL, NULL, 1);
    ASSERT_TRUE(runtime->register_pipeline(
        &blocked.pipeline, blocked.wait_sources()).ok());
    ASSERT_TRUE(runtime->register_pipeline(
        &completed.pipeline, completed.wait_sources()).ok());
    ASSERT_TRUE(runtime->start().ok());

    const eavp::Status stopped = runtime->stop();

    EXPECT_EQ(eavp::StatusCode::kTimeout, stopped.code());
    EXPECT_EQ(1, completed.node->stop_count());
    EXPECT_EQ(2, blocked_calls_at_first_sleep);
    EXPECT_EQ(1, raw_api->monotonic_sleep_count);
}

TEST(LinuxPlatformRuntimeTest,
     RetriesInterruptedDrainBackoffWithABoundedAbsoluteDeadline) {
    RuntimePipelineHarness harness(
        "interrupted-backoff", eavp::Status::ok_status(),
        eavp::Status::ok_status(), true);
    FakeLinuxRuntimeApi* raw_api = new FakeLinuxRuntimeApi();
    raw_api->set_monotonic_step_ns(1000L);
    raw_api->queue_sleep_error(EINTR);
    raw_api->queue_sleep_error(0);
    std::unique_ptr<eavp::detail::LinuxRuntimeApi> api(raw_api);
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime =
        create_injected_runtime(std::move(api), NULL, NULL, 1);
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());
    ASSERT_TRUE(runtime->start().ok());

    const eavp::Status stopped = runtime->stop();

    EXPECT_EQ(eavp::StatusCode::kTimeout, stopped.code());
    EXPECT_LE(harness.node->stop_count(), 2);
    EXPECT_EQ(2, raw_api->monotonic_sleep_count);
    ASSERT_EQ(2U, raw_api->sleep_deadlines.size());
    EXPECT_EQ(raw_api->sleep_deadlines[0].tv_sec,
              raw_api->sleep_deadlines[1].tv_sec);
    EXPECT_EQ(raw_api->sleep_deadlines[0].tv_nsec,
              raw_api->sleep_deadlines[1].tv_nsec);
}

TEST(LinuxPlatformRuntimeTest,
     BoundsRepeatedInterruptionsDuringDrainBackoff) {
    RuntimePipelineHarness harness(
        "bounded-interrupts", eavp::Status::ok_status(),
        eavp::Status::ok_status(), true);
    FakeLinuxRuntimeApi* raw_api = new FakeLinuxRuntimeApi();
    raw_api->set_monotonic_step_ns(1000L);
    for (int attempt = 0; attempt < 64; ++attempt) {
        raw_api->queue_sleep_error(EINTR);
    }
    std::unique_ptr<eavp::detail::LinuxRuntimeApi> api(raw_api);
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime =
        create_injected_runtime(std::move(api), NULL, NULL, 1);
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());
    ASSERT_TRUE(runtime->start().ok());

    const eavp::Status stopped = runtime->stop();

    EXPECT_EQ(eavp::StatusCode::kIoError, stopped.code());
    EXPECT_EQ("linux_runtime", stopped.provider_id());
    EXPECT_EQ("clock_nanosleep", stopped.operation());
    EXPECT_EQ(EINTR, stopped.native_code());
    EXPECT_EQ(64, raw_api->monotonic_sleep_count);
    EXPECT_EQ(1, harness.node->stop_count());
    EXPECT_GE(harness.node->reset_count(), 1);
}

TEST(LinuxPlatformRuntimeTest,
     DoesNotBackOffAfterTheDrainClockFails) {
    RuntimePipelineHarness harness(
        "clock-failure", eavp::Status::ok_status(),
        eavp::Status::ok_status(), true);
    FakeLinuxRuntimeApi* raw_api = new FakeLinuxRuntimeApi();
    raw_api->queue_monotonic_error(0);
    raw_api->queue_monotonic_error(EIO);
    std::unique_ptr<eavp::detail::LinuxRuntimeApi> api(raw_api);
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime =
        create_injected_runtime(std::move(api), NULL, NULL, 10);
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());
    ASSERT_TRUE(runtime->start().ok());

    const eavp::Status stopped = runtime->stop();

    EXPECT_EQ(eavp::StatusCode::kIoError, stopped.code());
    EXPECT_EQ("clock_gettime", stopped.operation());
    EXPECT_EQ(EIO, stopped.native_code());
    EXPECT_EQ(0, raw_api->monotonic_sleep_count);
    EXPECT_EQ(1, harness.node->stop_count());
}

TEST(LinuxPlatformRuntimeTest,
     DrainBackoffFailureDoesNotOverrideTheCancelRootCause) {
    RuntimePipelineHarness harness(
        "backoff-priority", eavp::Status::ok_status(),
        eavp::Status::ok_status(), true);
    harness.node->set_reset_status(eavp::Status(
        eavp::StatusCode::kDeviceLost, "cancel root cause",
        "camera", "reset", 29));
    FakeLinuxRuntimeApi* raw_api = new FakeLinuxRuntimeApi();
    raw_api->set_monotonic_step_ns(1000L);
    raw_api->queue_sleep_error(EIO);
    std::unique_ptr<eavp::detail::LinuxRuntimeApi> api(raw_api);
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime =
        create_injected_runtime(std::move(api), NULL, NULL, 10);
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());
    ASSERT_TRUE(runtime->start().ok());

    const eavp::Status stopped = runtime->stop();

    EXPECT_EQ(eavp::StatusCode::kDeviceLost, stopped.code());
    EXPECT_EQ("camera", stopped.provider_id());
    EXPECT_EQ("reset", stopped.operation());
    EXPECT_EQ(29, stopped.native_code());
    EXPECT_EQ(1, raw_api->monotonic_sleep_count);
}

TEST(LinuxPlatformRuntimeTest, DestructorStopsAndJoinsTheOwnedReactor) {
    RuntimePipelineHarness harness("destructor");
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime = create_runtime();
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());
    ASSERT_TRUE(runtime->start().ok());

    runtime.reset();

    EXPECT_EQ(eavp::PipelineState::kStopped, harness.pipeline.state());
    EXPECT_EQ(1, harness.node->stop_count());
    EXPECT_EQ(1, harness.node->reset_count());
}

TEST(LinuxPlatformRuntimeTest, TicksOnceWhenTwoSourcesForOnePipelineAreReady) {
    RuntimePipelineHarness harness("coalesced-runtime");
    PipeWaitSource second_source;
    harness.node->set_additional_ready_fd(second_source.read_fd());
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime = create_runtime();
    std::vector<eavp::LinuxWaitSource*> sources;
    sources.push_back(&harness.source);
    sources.push_back(&second_source);
    ASSERT_TRUE(runtime->register_pipeline(&harness.pipeline, sources).ok());
    ASSERT_TRUE(harness.source.signal());
    ASSERT_TRUE(second_source.signal());

    ASSERT_TRUE(runtime->start().ok());
    ASSERT_TRUE(harness.node->wait_for_ticks(1));
    ASSERT_TRUE(runtime->stop().ok());

    EXPECT_EQ(1, harness.node->tick_count());
}

TEST(LinuxPlatformRuntimeTest, PublishesStableRuntimeMetrics) {
    eavp::MetricRegistry metrics;
    const eavp::Status fatal(
        eavp::StatusCode::kDeviceLost, "metrics fatal", "camera", "tick", 7);
    RuntimePipelineHarness harness(
        "metrics", eavp::Status::ok_status(), fatal);
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime =
        create_runtime(100, &metrics);
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());
    ASSERT_TRUE(runtime->start().ok());

    ASSERT_TRUE(harness.source.signal());
    ASSERT_TRUE(harness.node->wait_for_ticks(1));
    EXPECT_EQ(eavp::StatusCode::kDeviceLost, runtime->stop().code());

    ASSERT_TRUE(metrics.counter("runtime.poll.wakeups").ok());
    EXPECT_GE(metrics.counter("runtime.poll.wakeups").value(), 1U);
    ASSERT_TRUE(metrics.counter("runtime.poll.interrupted").ok());
    EXPECT_EQ(0U, metrics.counter("runtime.poll.interrupted").value());
    ASSERT_TRUE(metrics.counter("runtime.pipeline.turns").ok());
    EXPECT_EQ(1U, metrics.counter("runtime.pipeline.turns").value());
    ASSERT_TRUE(metrics.counter("runtime.pipeline.failures").ok());
    EXPECT_EQ(1U, metrics.counter("runtime.pipeline.failures").value());
    ASSERT_TRUE(metrics.gauge("runtime.reactor.running").ok());
    EXPECT_DOUBLE_EQ(0.0,
                     metrics.gauge("runtime.reactor.running").value());
}

TEST(LinuxPlatformRuntimeTest, ReturnsAnObserverFailureWithoutThreadException) {
    FakeRuntimeObserver observer;
    observer.throw_on_turn_ = true;
    RuntimePipelineHarness harness("observer-only");
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime =
        create_test_runtime(&observer);
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());
    ASSERT_TRUE(runtime->start().ok());

    ASSERT_TRUE(harness.source.signal());
    ASSERT_TRUE(harness.node->wait_for_ticks(1));
    const eavp::Status stopped = runtime->stop();

    EXPECT_EQ(eavp::StatusCode::kInternal, stopped.code());
    EXPECT_EQ(eavp::PlatformRuntimeState::kError, runtime->state());
    EXPECT_EQ(eavp::StatusCode::kInternal,
              runtime->last_failure().code());
    EXPECT_EQ(1, observer.turn_count_);
    EXPECT_EQ(1, observer.running_true_count_);
    EXPECT_EQ(1, observer.running_false_count_);
}

TEST(LinuxPlatformRuntimeTest, PreservesMediaFailureOverObserverFailure) {
    FakeRuntimeObserver observer;
    observer.turn_status_ = eavp::Status(
        eavp::StatusCode::kResourceExhausted, "observer failed");
    observer.failure_status_ = eavp::Status(
        eavp::StatusCode::kInternal, "failure metric failed");
    const eavp::Status media_failure(
        eavp::StatusCode::kDeviceLost, "camera lost", "camera", "tick", 23);
    RuntimePipelineHarness harness(
        "priority", eavp::Status::ok_status(), media_failure);
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime =
        create_test_runtime(&observer);
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());
    ASSERT_TRUE(runtime->start().ok());

    ASSERT_TRUE(harness.source.signal());
    ASSERT_TRUE(harness.node->wait_for_ticks(1));
    const eavp::Status stopped = runtime->stop();

    EXPECT_EQ(eavp::StatusCode::kDeviceLost, stopped.code());
    EXPECT_EQ("camera", stopped.provider_id());
    EXPECT_EQ("tick", stopped.operation());
    EXPECT_EQ(23, stopped.native_code());
    EXPECT_EQ(eavp::StatusCode::kDeviceLost,
              runtime->last_failure().code());
    EXPECT_EQ(1, observer.failure_count_);
}

TEST(LinuxPlatformRuntimeTest, JoinsWhenTheRunningMetricFailsDuringStart) {
    FakeRuntimeObserver observer;
    observer.running_status_ = eavp::Status(
        eavp::StatusCode::kResourceExhausted, "running metric failed");
    RuntimePipelineHarness harness("running-observer");
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime =
        create_test_runtime(&observer);
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());

    const eavp::Status started = runtime->start();

    EXPECT_EQ(eavp::StatusCode::kResourceExhausted, started.code());
    EXPECT_EQ(eavp::PlatformRuntimeState::kError, runtime->state());
    EXPECT_EQ(1, harness.node->reset_count());
    EXPECT_EQ(1, observer.running_true_count_);
    EXPECT_EQ(1, observer.running_false_count_);
    EXPECT_EQ(started.code(), runtime->stop().code());
}

TEST(LinuxPlatformRuntimeTest, WaitsForClaimedWakeBeforeClosingEventLoop) {
    RuntimeConcurrencyProbe probe;
    probe.block_wake_claim_ = true;
    probe.block_close_pending_ = true;
    probe.block_write_ = true;
    const eavp::Status fatal(
        eavp::StatusCode::kDeviceLost, "wake race", "camera", "tick", 31);
    RuntimePipelineHarness harness(
        "wake-close", eavp::Status::ok_status(), fatal);
    harness.node->block_tick_exit();
    std::unique_ptr<eavp::detail::LinuxRuntimeApi> api(
        new InstrumentedLinuxRuntimeApi(&probe));
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime =
        create_injected_runtime(std::move(api), NULL, &probe);
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());
    ASSERT_TRUE(runtime->start().ok());
    ASSERT_TRUE(harness.source.signal());
    ASSERT_TRUE(harness.node->wait_for_ticks(1));

    eavp::Status stopped(eavp::StatusCode::kInvalidState);
    std::thread stopper([&]() { stopped = runtime->stop(); });
    const bool wake_claimed = probe.wait_for_wake_claimed();
    EXPECT_TRUE(wake_claimed);
    harness.node->allow_tick_exit();
    const bool close_pending = probe.wait_for_close_pending();
    EXPECT_TRUE(close_pending);
    probe.allow_wake_claim();
    const bool write_entered = probe.wait_for_write_entered();
    EXPECT_TRUE(write_entered);
    probe.allow_close_pending();
    const bool close_or_wait = probe.wait_for_close_or_wake_wait();
    EXPECT_TRUE(close_or_wait);

    EXPECT_TRUE(probe.reactor_waiting_for_wake());
    EXPECT_FALSE(probe.close_while_write());

    probe.allow_write();
    probe.allow_everything();
    stopper.join();
    EXPECT_EQ(eavp::StatusCode::kDeviceLost, stopped.code());
}

TEST(LinuxPlatformRuntimeTest, ConcurrentStopsShareOneJoinOwner) {
    RuntimeConcurrencyProbe probe;
    probe.block_thread_finish_ = true;
    probe.block_join_owner_ = true;
    RuntimePipelineHarness harness("double-stop");
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime =
        create_injected_runtime(
            eavp::detail::create_linux_runtime_api(), NULL, &probe);
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());
    ASSERT_TRUE(runtime->start().ok());

    std::mutex callers_mutex;
    std::condition_variable callers_condition;
    int ready_callers = 0;
    int returned_callers = 0;
    bool call_stop = false;
    eavp::Status first(eavp::StatusCode::kInvalidState);
    eavp::Status second(eavp::StatusCode::kInvalidState);
    const auto stop_call = [&](eavp::Status* result) {
        {
            std::unique_lock<std::mutex> lock(callers_mutex);
            ++ready_callers;
            callers_condition.notify_all();
            callers_condition.wait(lock, [&]() { return call_stop; });
        }
        *result = runtime->stop();
        {
            std::lock_guard<std::mutex> lock(callers_mutex);
            ++returned_callers;
            callers_condition.notify_all();
        }
    };
    std::thread first_stopper(stop_call, &first);
    std::thread second_stopper(stop_call, &second);
    {
        std::unique_lock<std::mutex> lock(callers_mutex);
        const bool both_callers_ready = callers_condition.wait_for(
            lock, std::chrono::seconds(2),
            [&]() { return ready_callers == 2; });
        EXPECT_TRUE(both_callers_ready);
        call_stop = true;
        callers_condition.notify_all();
    }

    const bool two_join_participants = probe.wait_for_join_participants(2);
    EXPECT_TRUE(two_join_participants);
    EXPECT_EQ(1, probe.join_owner_count());
    EXPECT_EQ(1, probe.join_waiter_count());
    {
        std::lock_guard<std::mutex> lock(callers_mutex);
        EXPECT_EQ(0, returned_callers);
    }

    probe.allow_thread_and_first_join_owner();
    {
        std::unique_lock<std::mutex> lock(callers_mutex);
        EXPECT_TRUE(callers_condition.wait_for(
            lock, std::chrono::seconds(2),
            [&]() { return returned_callers >= 1; }));
    }
    probe.allow_later_join_owners();
    probe.allow_everything();
    first_stopper.join();
    second_stopper.join();
    EXPECT_TRUE(first.ok());
    EXPECT_TRUE(second.ok());
}

TEST(LinuxPlatformRuntimeTest, StoppedStateStillWaitsForExistingJoinOwner) {
    RuntimeConcurrencyProbe probe;
    probe.block_thread_finish_ = true;
    probe.block_join_owner_ = true;
    RuntimePipelineHarness harness("stopped-join-gate");
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime =
        create_injected_runtime(
            eavp::detail::create_linux_runtime_api(), NULL, &probe);
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());
    ASSERT_TRUE(runtime->start().ok());

    eavp::Status first(eavp::StatusCode::kInvalidState);
    eavp::Status second(eavp::StatusCode::kInvalidState);
    std::mutex second_mutex;
    std::condition_variable second_condition;
    bool second_entered = false;
    bool second_returned = false;
    std::thread first_stopper([&]() { first = runtime->stop(); });

    EXPECT_TRUE(probe.wait_for_join_participants(1));
    EXPECT_TRUE(probe.wait_for_thread_finishing());
    EXPECT_EQ(eavp::PlatformRuntimeState::kStopped, runtime->state());

    std::thread second_stopper([&]() {
        {
            std::lock_guard<std::mutex> lock(second_mutex);
            second_entered = true;
            second_condition.notify_all();
        }
        second = runtime->stop();
        {
            std::lock_guard<std::mutex> lock(second_mutex);
            second_returned = true;
            second_condition.notify_all();
        }
    });
    {
        std::unique_lock<std::mutex> lock(second_mutex);
        EXPECT_TRUE(second_condition.wait_for(
            lock, std::chrono::seconds(2),
            [&]() { return second_entered; }));
    }

    EXPECT_TRUE(probe.wait_for_join_participants(2));
    {
        std::lock_guard<std::mutex> lock(second_mutex);
        EXPECT_FALSE(second_returned);
    }

    probe.allow_everything();
    first_stopper.join();
    second_stopper.join();
    EXPECT_TRUE(first.ok());
    EXPECT_TRUE(second.ok());
}

TEST(LinuxPlatformRuntimeTest, StartFailureAndStopShareOneJoinOwner) {
    RuntimeConcurrencyProbe probe;
    probe.block_thread_finish_ = true;
    probe.block_join_owner_ = true;
    RuntimePipelineHarness harness(
        "start-stop-join",
        eavp::Status(eavp::StatusCode::kDeviceLost, "start lost"));
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime =
        create_injected_runtime(
            eavp::detail::create_linux_runtime_api(), NULL, &probe);
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());

    eavp::Status started(eavp::StatusCode::kInvalidState);
    eavp::Status stopped(eavp::StatusCode::kInvalidState);
    std::mutex callers_mutex;
    std::condition_variable callers_condition;
    int returned_callers = 0;
    std::thread starter([&]() {
        started = runtime->start();
        std::lock_guard<std::mutex> lock(callers_mutex);
        ++returned_callers;
        callers_condition.notify_all();
    });
    const bool thread_finishing = probe.wait_for_thread_finishing();
    EXPECT_TRUE(thread_finishing);
    std::thread stopper([&]() {
        stopped = runtime->stop();
        std::lock_guard<std::mutex> lock(callers_mutex);
        ++returned_callers;
        callers_condition.notify_all();
    });

    const bool two_join_participants = probe.wait_for_join_participants(2);
    EXPECT_TRUE(two_join_participants);
    EXPECT_EQ(1, probe.join_owner_count());
    EXPECT_EQ(1, probe.join_waiter_count());

    probe.allow_thread_and_first_join_owner();
    {
        std::unique_lock<std::mutex> lock(callers_mutex);
        EXPECT_TRUE(callers_condition.wait_for(
            lock, std::chrono::seconds(2),
            [&]() { return returned_callers >= 1; }));
    }
    probe.allow_later_join_owners();
    probe.allow_everything();
    starter.join();
    stopper.join();
    EXPECT_EQ(eavp::StatusCode::kDeviceLost, started.code());
    EXPECT_EQ(eavp::StatusCode::kDeviceLost, stopped.code());
}

TEST(LinuxPlatformRuntimeTest,
     StartReturnsLatchedSuccessWhenConcurrentStopFinishesFirst) {
    StartupResultProbe probe;
    RuntimePipelineHarness harness("startup-result-latch");
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime =
        create_injected_runtime(
            eavp::detail::create_linux_runtime_api(), NULL, &probe);
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());

    eavp::Status started(eavp::StatusCode::kInvalidState);
    std::thread starter([&]() { started = runtime->start(); });
    const bool startup_result_ready = probe.wait_until_entered();
    if (!startup_result_ready) {
        const eavp::Status stopped = runtime->stop();
        starter.join();
        EXPECT_TRUE(startup_result_ready);
        EXPECT_TRUE(stopped.ok());
        return;
    }

    const eavp::Status stopped = runtime->stop();
    probe.release();
    starter.join();

    EXPECT_TRUE(stopped.ok());
    EXPECT_TRUE(started.ok());
    EXPECT_EQ(eavp::StatusCode::kOk, started.code());
    EXPECT_EQ(eavp::PlatformRuntimeState::kStopped, runtime->state());
}

TEST(LinuxPlatformRuntimeTest, WaitSourceFailureRemainsTheFirstMediaCause) {
    FakeRuntimeObserver observer;
    RuntimePipelineHarness harness("wait-source-failure");
    const eavp::Status source_failure(
        eavp::StatusCode::kDeviceLost, "source lost", "linux_runtime", "poll",
        41);
    harness.source.set_evaluate_failure(source_failure);
    harness.node->set_reset_status(eavp::Status(
        eavp::StatusCode::kCorruptData, "cancel failed"));
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime =
        create_test_runtime(&observer);
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());
    ASSERT_TRUE(runtime->start().ok());

    ASSERT_TRUE(harness.source.signal());
    const eavp::Status stopped = runtime->stop();

    EXPECT_EQ(eavp::StatusCode::kDeviceLost, stopped.code());
    EXPECT_EQ("linux_runtime", stopped.provider_id());
    EXPECT_EQ("poll", stopped.operation());
    EXPECT_EQ(41, stopped.native_code());
    EXPECT_EQ(1, observer.failure_count_);
}

TEST(LinuxPlatformRuntimeTest,
     ProviderlessEventLoopExceptionsRemainReactorFailures) {
    const ControlledWaitFailureApi::FailureMode modes[] = {
        ControlledWaitFailureApi::kBadAlloc,
        ControlledWaitFailureApi::kInternal};
    const eavp::StatusCode expected_codes[] = {
        eavp::StatusCode::kResourceExhausted,
        eavp::StatusCode::kInternal};

    for (std::size_t index = 0U; index < 2U; ++index) {
        FakeRuntimeObserver observer;
        RuntimePipelineHarness harness(
            index == 0U ? "wait-bad-alloc" : "wait-internal");
        ControlledWaitFailureApi* raw_api =
            new ControlledWaitFailureApi(modes[index], true);
        std::unique_ptr<eavp::detail::LinuxRuntimeApi> api(raw_api);
        std::unique_ptr<eavp::LinuxPlatformRuntime> runtime =
            create_injected_runtime(std::move(api), &observer, NULL);
        ASSERT_TRUE(runtime->register_pipeline(
            &harness.pipeline, harness.wait_sources()).ok());
        ASSERT_TRUE(runtime->start().ok());

        const bool wait_entered = raw_api->wait_for_wait_entered();
        EXPECT_TRUE(wait_entered);
        raw_api->allow_wait();
        const eavp::Status stopped = runtime->stop();

        EXPECT_EQ(expected_codes[index], stopped.code());
        EXPECT_TRUE(stopped.provider_id().empty());
        EXPECT_EQ(0, observer.failure_count_);
    }
}

TEST(LinuxPlatformRuntimeTest, RejectsAnUnrepresentableStopDeadline) {
    RuntimePipelineHarness harness(
        "deadline-overflow", eavp::Status::ok_status(),
        eavp::Status::ok_status(), true);
    InstrumentedLinuxRuntimeApi* raw_api =
        new InstrumentedLinuxRuntimeApi();
    struct timespec near_limit;
    near_limit.tv_sec = std::numeric_limits<time_t>::max();
    near_limit.tv_nsec = 999999999L;
    raw_api->set_fixed_time(near_limit);
    std::unique_ptr<eavp::detail::LinuxRuntimeApi> api(raw_api);
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime =
        create_injected_runtime(std::move(api), NULL, NULL, 100);
    ASSERT_TRUE(runtime->register_pipeline(
        &harness.pipeline, harness.wait_sources()).ok());
    ASSERT_TRUE(runtime->start().ok());

    const eavp::Status stopped = runtime->stop();

    EXPECT_EQ(eavp::StatusCode::kResourceExhausted, stopped.code());
    EXPECT_EQ(eavp::PlatformRuntimeState::kError, runtime->state());
    EXPECT_GE(harness.node->reset_count(), 1);
}

TEST(LinuxPlatformRuntimeTest, SnapshotAccessorsContainInjectedExceptions) {
    RuntimeConcurrencyProbe probe;
    std::unique_ptr<eavp::LinuxPlatformRuntime> runtime =
        create_injected_runtime(
            eavp::detail::create_linux_runtime_api(), NULL, &probe);

    probe.throw_bad_alloc_on_state_ = true;
    EXPECT_EQ(eavp::PlatformRuntimeState::kError, runtime->state());
    probe.throw_bad_alloc_on_state_ = false;
    probe.throw_unknown_on_state_ = true;
    EXPECT_EQ(eavp::PlatformRuntimeState::kError, runtime->state());
    probe.throw_unknown_on_state_ = false;

    probe.throw_bad_alloc_on_last_failure_ = true;
    EXPECT_EQ(eavp::StatusCode::kResourceExhausted,
              runtime->last_failure().code());
    probe.throw_bad_alloc_on_last_failure_ = false;
    probe.throw_unknown_on_last_failure_ = true;
    EXPECT_EQ(eavp::StatusCode::kInternal,
              runtime->last_failure().code());
}

TEST(LinuxEventLoopTest, CoalescesReadySourcesForTheSamePipeline) {
    RuntimeFixture fixture;
    fixture.source_a.set_descriptors(std::vector<pollfd>(1, readable_fd(10)));
    fixture.source_b.set_descriptors(std::vector<pollfd>(1, readable_fd(11)));
    fixture.api->queue_ready_fds(std::vector<int>{10, 11});

    ASSERT_TRUE(fixture.loop->initialize().ok());
    ASSERT_TRUE(fixture.loop->register_source(
        &fixture.pipeline, &fixture.source_a).ok());
    ASSERT_TRUE(fixture.loop->register_source(
        &fixture.pipeline, &fixture.source_b).ok());
    const eavp::Result<eavp::detail::LinuxEventLoopTurn> ready =
        fixture.loop->wait_once();

    ASSERT_TRUE(ready.ok());
    ASSERT_EQ(1U, ready.value().ready_pipelines.size());
    EXPECT_EQ(&fixture.pipeline, ready.value().ready_pipelines[0]);
    EXPECT_EQ(1U, ready.value().wakeup_count);
    EXPECT_EQ(0U, ready.value().interrupted_count);
    EXPECT_FALSE(ready.value().control_wakeup);
}

TEST(LinuxEventLoopTest, ResetsExplicitWaitFailureOriginOnEveryWait) {
    RuntimeFixture fixture;

    EXPECT_FALSE(fixture.loop->wait_once().ok());
    EXPECT_EQ(eavp::detail::LinuxEventLoopWaitFailureOrigin::kRuntime,
              fixture.loop->wait_failure_origin());

    ASSERT_TRUE(fixture.loop->initialize().ok());
    fixture.api->queue_events(std::vector<FakeLinuxRuntimeApi::ReadyEvent>());
    EXPECT_TRUE(fixture.loop->wait_once().ok());
    EXPECT_EQ(eavp::detail::LinuxEventLoopWaitFailureOrigin::kNone,
              fixture.loop->wait_failure_origin());
}

TEST(LinuxEventLoopTest,
     MarksProviderlessExceptionsAsRuntimeWaitFailures) {
    const ControlledWaitFailureApi::FailureMode modes[] = {
        ControlledWaitFailureApi::kBadAlloc,
        ControlledWaitFailureApi::kInternal};
    const eavp::StatusCode expected_codes[] = {
        eavp::StatusCode::kResourceExhausted,
        eavp::StatusCode::kInternal};

    for (std::size_t index = 0U; index < 2U; ++index) {
        ControlledWaitFailureApi* raw_api =
            new ControlledWaitFailureApi(modes[index], false);
        std::unique_ptr<eavp::detail::LinuxRuntimeApi> api(raw_api);
        eavp::detail::LinuxEventLoop loop(std::move(api));
        ASSERT_TRUE(loop.initialize().ok());

        const eavp::Result<eavp::detail::LinuxEventLoopTurn> ready =
            loop.wait_once();

        ASSERT_FALSE(ready.ok());
        EXPECT_EQ(expected_codes[index], ready.status().code());
        EXPECT_TRUE(ready.status().provider_id().empty());
        EXPECT_EQ(eavp::detail::LinuxEventLoopWaitFailureOrigin::kRuntime,
                  loop.wait_failure_origin());
        EXPECT_TRUE(loop.wake().ok());
        EXPECT_EQ(eavp::detail::LinuxEventLoopWaitFailureOrigin::kRuntime,
                  loop.wait_failure_origin());
    }
}

TEST(LinuxEventLoopTest,
     MarksEvaluateFailureAsWaitSourceRegardlessOfProvider) {
    RuntimeFixture fixture;
    fixture.source_a.set_descriptors(
        std::vector<pollfd>(1, readable_fd(10)));
    fixture.source_a.set_evaluate_failure(eavp::Status(
        eavp::StatusCode::kDeviceLost, "source lost", "linux_runtime",
        "evaluate", 51));
    fixture.api->queue_ready_fds(std::vector<int>(1, 10));
    ASSERT_TRUE(fixture.loop->initialize().ok());
    ASSERT_TRUE(fixture.loop->register_source(
        &fixture.pipeline, &fixture.source_a).ok());

    const eavp::Result<eavp::detail::LinuxEventLoopTurn> ready =
        fixture.loop->wait_once();

    ASSERT_FALSE(ready.ok());
    EXPECT_EQ(eavp::StatusCode::kDeviceLost, ready.status().code());
    EXPECT_EQ("source lost", ready.status().message());
    EXPECT_EQ("linux_runtime", ready.status().provider_id());
    EXPECT_EQ("evaluate", ready.status().operation());
    EXPECT_EQ(51, ready.status().native_code());
    EXPECT_EQ(eavp::detail::LinuxEventLoopWaitFailureOrigin::kWaitSource,
              fixture.loop->wait_failure_origin());
}

TEST(LinuxEventLoopTest, RegistersLevelTriggeredPollInterestsExactly) {
    RuntimeFixture fixture;
    const struct pollfd input = {10, static_cast<short>(POLLIN), 0};
    const struct pollfd normal_input = {
        12, static_cast<short>(POLLRDNORM), 0};
    const struct pollfd output_priority = {
        11, static_cast<short>(POLLOUT | POLLPRI), 0};
    fixture.source_a.set_descriptors(
        std::vector<pollfd>{input, normal_input, output_priority});

    ASSERT_TRUE(fixture.loop->initialize().ok());
    ASSERT_TRUE(fixture.loop->register_source(
        &fixture.pipeline, &fixture.source_a).ok());

    ASSERT_EQ(4U, fixture.api->add_calls.size());
    EXPECT_EQ(EPOLLIN, fixture.api->add_calls[0].events);
    EXPECT_EQ(EPOLLIN, fixture.api->add_calls[1].events);
    EXPECT_EQ(EPOLLIN, fixture.api->add_calls[2].events);
    EXPECT_EQ(static_cast<std::uint32_t>(EPOLLOUT | EPOLLPRI),
              fixture.api->add_calls[3].events);
    EXPECT_EQ(0U, fixture.api->add_calls[1].events &
                      static_cast<std::uint32_t>(EPOLLET | EPOLLONESHOT));
    EXPECT_EQ(0U, fixture.api->add_calls[2].events &
                      static_cast<std::uint32_t>(EPOLLET | EPOLLONESHOT));
    EXPECT_EQ(0U, fixture.api->add_calls[3].events &
                      static_cast<std::uint32_t>(EPOLLET | EPOLLONESHOT));

    fixture.api->queue_ready_fds(std::vector<int>(1U, 12));
    const eavp::Result<eavp::detail::LinuxEventLoopTurn> ready =
        fixture.loop->wait_once();
    ASSERT_TRUE(ready.ok());
    ASSERT_EQ(3U, fixture.source_a.evaluated_descriptors().size());
    EXPECT_EQ(static_cast<short>(POLLRDNORM),
              fixture.source_a.evaluated_descriptors()[1].revents);
    ASSERT_EQ(1U, ready.value().ready_pipelines.size());
}

TEST(LinuxEventLoopTest, RejectsEmptyInvalidAndDuplicateDescriptors) {
    RuntimeFixture fixture;
    ASSERT_TRUE(fixture.loop->initialize().ok());

    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              fixture.loop->register_source(
                  &fixture.pipeline, &fixture.source_a).code());

    const struct pollfd unsupported = {10, static_cast<short>(POLLNVAL), 0};
    fixture.source_a.set_descriptors(std::vector<pollfd>(1, unsupported));
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              fixture.loop->register_source(
                  &fixture.pipeline, &fixture.source_a).code());

    fixture.source_a.set_descriptors(std::vector<pollfd>(1, readable_fd(10)));
    fixture.source_b.set_descriptors(std::vector<pollfd>(1, readable_fd(10)));
    ASSERT_TRUE(fixture.loop->register_source(
        &fixture.pipeline, &fixture.source_a).ok());
    EXPECT_EQ(eavp::StatusCode::kAlreadyExists,
              fixture.loop->register_source(
                  &fixture.pipeline, &fixture.source_b).code());
}

TEST(LinuxEventLoopTest, RejectsEveryInvalidAndDuplicateDescriptorShape) {
    RuntimeFixture fixture;
    ASSERT_TRUE(fixture.loop->initialize().ok());

    const struct pollfd negative = {-1, static_cast<short>(POLLIN), 0};
    fixture.source_a.set_descriptors(std::vector<pollfd>(1, negative));
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              fixture.loop->register_source(
                  &fixture.pipeline, &fixture.source_a).code());

    const struct pollfd no_events = {10, 0, 0};
    fixture.source_a.set_descriptors(std::vector<pollfd>(1, no_events));
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              fixture.loop->register_source(
                  &fixture.pipeline, &fixture.source_a).code());

    fixture.source_a.set_descriptors(
        std::vector<pollfd>{readable_fd(10), readable_fd(10)});
    EXPECT_EQ(eavp::StatusCode::kAlreadyExists,
              fixture.loop->register_source(
                  &fixture.pipeline, &fixture.source_a).code());

    fixture.source_a.set_descriptors(std::vector<pollfd>(1, readable_fd(10)));
    ASSERT_TRUE(fixture.loop->register_source(
        &fixture.pipeline, &fixture.source_a).ok());
    EXPECT_EQ(eavp::StatusCode::kAlreadyExists,
              fixture.loop->register_source(
                  &fixture.pipeline, &fixture.source_a).code());
}

TEST(LinuxEventLoopTest, RemovesPartialKernelRegistrationAndAllowsRetry) {
    RuntimeFixture fixture;
    fixture.source_a.set_descriptors(std::vector<pollfd>{
        readable_fd(10), readable_fd(11), readable_fd(12)});
    ASSERT_TRUE(fixture.loop->initialize().ok());
    fixture.api->fail_epoll_add_call = 4;
    fixture.api->fail_epoll_add_error = EIO;

    const eavp::Status failed = fixture.loop->register_source(
        &fixture.pipeline, &fixture.source_a);

    EXPECT_EQ(eavp::StatusCode::kIoError, failed.code());
    EXPECT_EQ(EIO, failed.native_code());
    ASSERT_EQ(2U, fixture.api->remove_calls.size());
    EXPECT_EQ(11, fixture.api->remove_calls[0].fd);
    EXPECT_EQ(10, fixture.api->remove_calls[1].fd);
    EXPECT_TRUE(fixture.loop->register_source(
        &fixture.pipeline, &fixture.source_a).ok());
}

TEST(LinuxEventLoopTest, InvalidatesLoopWhenRegistrationRollbackFails) {
    RuntimeFixture fixture;
    fixture.source_a.set_descriptors(
        std::vector<pollfd>{readable_fd(10), readable_fd(11)});
    ASSERT_TRUE(fixture.loop->initialize().ok());
    fixture.api->fail_epoll_add_call = 3;
    fixture.api->fail_epoll_add_error = EIO;
    fixture.api->epoll_remove_result = -1;
    fixture.api->epoll_remove_error = EBADF;

    const eavp::Status failed = fixture.loop->register_source(
        &fixture.pipeline, &fixture.source_a);

    EXPECT_EQ(eavp::StatusCode::kIoError, failed.code());
    EXPECT_EQ(EIO, failed.native_code());
    ASSERT_EQ(1U, fixture.api->remove_calls.size());
    EXPECT_EQ(10, fixture.api->remove_calls[0].fd);
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              fixture.loop->wait_once().status().code());
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              fixture.loop->initialize().code());
    EXPECT_EQ(1, fixture.api->close_count_for(40));
    EXPECT_EQ(1, fixture.api->close_count_for(41));
}

TEST(LinuxEventLoopTest, RestoresUninitializedStateWhenInitializationThrows) {
    RuntimeFixture create_fixture;
    create_fixture.api->throw_on_epoll_create = true;
    EXPECT_EQ(eavp::StatusCode::kInternal,
              create_fixture.loop->initialize().code());
    EXPECT_TRUE(create_fixture.api->closed_fds.empty());
    create_fixture.api->throw_on_epoll_create = false;
    EXPECT_TRUE(create_fixture.loop->initialize().ok());

    RuntimeFixture event_fixture;
    event_fixture.api->throw_on_create_event_fd = true;

    const eavp::Status failed = event_fixture.loop->initialize();

    EXPECT_EQ(eavp::StatusCode::kResourceExhausted, failed.code());
    EXPECT_EQ(1, event_fixture.api->close_count_for(40));
    event_fixture.api->throw_on_create_event_fd = false;
    EXPECT_TRUE(event_fixture.loop->initialize().ok());

    RuntimeFixture add_fixture;
    add_fixture.api->throw_on_epoll_add_call = 1;
    EXPECT_EQ(eavp::StatusCode::kInternal,
              add_fixture.loop->initialize().code());
    EXPECT_EQ(1, add_fixture.api->close_count_for(40));
    EXPECT_EQ(1, add_fixture.api->close_count_for(41));
    EXPECT_TRUE(add_fixture.loop->initialize().ok());
}

TEST(LinuxEventLoopTest, RollsBackRegistrationWhenApiThrows) {
    RuntimeFixture allocation_fixture;
    allocation_fixture.source_a.set_descriptors(
        std::vector<pollfd>{readable_fd(10), readable_fd(11)});
    ASSERT_TRUE(allocation_fixture.loop->initialize().ok());
    allocation_fixture.api->throw_bad_alloc_on_epoll_add_call = 3;

    const eavp::Status failed = allocation_fixture.loop->register_source(
        &allocation_fixture.pipeline, &allocation_fixture.source_a);

    EXPECT_EQ(eavp::StatusCode::kResourceExhausted, failed.code());
    ASSERT_EQ(1U, allocation_fixture.api->remove_calls.size());
    EXPECT_EQ(10, allocation_fixture.api->remove_calls[0].fd);
    EXPECT_TRUE(allocation_fixture.loop->register_source(
        &allocation_fixture.pipeline, &allocation_fixture.source_a).ok());

    RuntimeFixture unknown_fixture;
    unknown_fixture.source_a.set_descriptors(
        std::vector<pollfd>{readable_fd(20), readable_fd(21)});
    ASSERT_TRUE(unknown_fixture.loop->initialize().ok());
    unknown_fixture.api->throw_on_epoll_add_call = 3;
    const eavp::Status unknown = unknown_fixture.loop->register_source(
        &unknown_fixture.pipeline, &unknown_fixture.source_a);
    EXPECT_EQ(eavp::StatusCode::kInternal, unknown.code());
    ASSERT_EQ(1U, unknown_fixture.api->remove_calls.size());
    EXPECT_EQ(20, unknown_fixture.api->remove_calls[0].fd);
    EXPECT_TRUE(unknown_fixture.loop->register_source(
        &unknown_fixture.pipeline, &unknown_fixture.source_a).ok());
}

TEST(LinuxEventLoopTest, EvaluatesEachSourceOnceAndHonorsFalseReadiness) {
    RuntimeFixture fixture;
    fixture.source_a.set_descriptors(
        std::vector<pollfd>{readable_fd(10), readable_fd(11)});
    fixture.source_a.set_evaluate_result(false);
    fixture.api->queue_ready_fds(std::vector<int>{10, 11});
    ASSERT_TRUE(fixture.loop->initialize().ok());
    ASSERT_TRUE(fixture.loop->register_source(
        &fixture.pipeline, &fixture.source_a).ok());

    const eavp::Result<eavp::detail::LinuxEventLoopTurn> ready =
        fixture.loop->wait_once();

    ASSERT_TRUE(ready.ok());
    EXPECT_TRUE(ready.value().ready_pipelines.empty());
    EXPECT_EQ(1, fixture.source_a.evaluate_call_count());
    ASSERT_EQ(2U, fixture.source_a.evaluated_descriptors().size());
    EXPECT_EQ(POLLIN, fixture.source_a.evaluated_descriptors()[0].revents);
    EXPECT_EQ(POLLIN, fixture.source_a.evaluated_descriptors()[1].revents);
}

TEST(LinuxEventLoopTest, ClearsStaleReventsAndForwardsErrorAndHangup) {
    RuntimeFixture fixture;
    const struct pollfd stale = {
        10, static_cast<short>(POLLIN), static_cast<short>(POLLNVAL)};
    fixture.source_a.set_descriptors(std::vector<pollfd>(1, stale));
    fixture.api->queue_events(std::vector<FakeLinuxRuntimeApi::ReadyEvent>(
        1, FakeLinuxRuntimeApi::ReadyEvent(10, EPOLLERR | EPOLLHUP)));
    ASSERT_TRUE(fixture.loop->initialize().ok());
    ASSERT_TRUE(fixture.loop->register_source(
        &fixture.pipeline, &fixture.source_a).ok());

    const eavp::Result<eavp::detail::LinuxEventLoopTurn> ready =
        fixture.loop->wait_once();

    ASSERT_TRUE(ready.ok());
    ASSERT_EQ(1U, ready.value().ready_pipelines.size());
    ASSERT_EQ(1U, fixture.source_a.evaluated_descriptors().size());
    EXPECT_EQ(static_cast<short>(POLLERR | POLLHUP),
              fixture.source_a.evaluated_descriptors()[0].revents);
}

TEST(LinuxEventLoopTest, IgnoresSpuriousTokens) {
    RuntimeFixture fixture;
    fixture.source_a.set_descriptors(std::vector<pollfd>(1, readable_fd(10)));
    fixture.api->queue_events(std::vector<FakeLinuxRuntimeApi::ReadyEvent>(
        1, FakeLinuxRuntimeApi::ReadyEvent::spurious_token(999U, EPOLLIN)));
    ASSERT_TRUE(fixture.loop->initialize().ok());
    ASSERT_TRUE(fixture.loop->register_source(
        &fixture.pipeline, &fixture.source_a).ok());

    const eavp::Result<eavp::detail::LinuxEventLoopTurn> ready =
        fixture.loop->wait_once();

    ASSERT_TRUE(ready.ok());
    EXPECT_TRUE(ready.value().ready_pipelines.empty());
    EXPECT_EQ(0, fixture.source_a.evaluate_call_count());
    EXPECT_EQ(1U, ready.value().wakeup_count);
}

TEST(LinuxEventLoopTest, WritesAndConsumesOnlyTheControlWakeToken) {
    RuntimeFixture fixture;
    fixture.source_a.set_descriptors(std::vector<pollfd>(1, readable_fd(10)));
    ASSERT_TRUE(fixture.loop->initialize().ok());
    ASSERT_TRUE(fixture.loop->register_source(
        &fixture.pipeline, &fixture.source_a).ok());
    ASSERT_TRUE(fixture.loop->wake().ok());
    fixture.api->event_fd_value = 7U;
    fixture.api->queue_ready_fds(std::vector<int>(1, fixture.api->event_fd_result));

    const eavp::Result<eavp::detail::LinuxEventLoopTurn> ready =
        fixture.loop->wait_once();

    ASSERT_TRUE(ready.ok());
    EXPECT_TRUE(ready.value().control_wakeup);
    EXPECT_TRUE(ready.value().ready_pipelines.empty());
    EXPECT_EQ(1U, ready.value().wakeup_count);
    EXPECT_EQ(1, fixture.api->write_event_fd_count);
    EXPECT_EQ(1U, fixture.api->written_value);
    EXPECT_EQ(1, fixture.api->read_event_fd_count);
    EXPECT_EQ(0, fixture.source_a.evaluate_call_count());
}

TEST(LinuxEventLoopTest, RetriesAtMostSixtyFourConsecutiveInterruptions) {
    RuntimeFixture fixture;
    ASSERT_TRUE(fixture.loop->initialize().ok());
    for (int count = 0; count < 64; ++count) fixture.api->queue_error(EINTR);

    const eavp::Result<eavp::detail::LinuxEventLoopTurn> ready =
        fixture.loop->wait_once();

    ASSERT_FALSE(ready.ok());
    EXPECT_EQ(eavp::StatusCode::kIoError, ready.status().code());
    EXPECT_EQ("Linux Reactor 等待被连续信号中断", ready.status().message());
    EXPECT_EQ("linux_runtime", ready.status().provider_id());
    EXPECT_EQ("epoll_wait", ready.status().operation());
    EXPECT_EQ(EINTR, ready.status().native_code());
    EXPECT_EQ(64, fixture.api->epoll_wait_count);
}

TEST(LinuxEventLoopTest, ReportsInterruptionsBeforeASuccessfulWait) {
    RuntimeFixture fixture;
    ASSERT_TRUE(fixture.loop->initialize().ok());
    for (int count = 0; count < 63; ++count) fixture.api->queue_error(EINTR);
    fixture.api->queue_events(std::vector<FakeLinuxRuntimeApi::ReadyEvent>());

    const eavp::Result<eavp::detail::LinuxEventLoopTurn> ready =
        fixture.loop->wait_once();

    ASSERT_TRUE(ready.ok());
    EXPECT_EQ(63U, ready.value().interrupted_count);
    EXPECT_EQ(1U, ready.value().wakeup_count);
    EXPECT_EQ(64, fixture.api->epoll_wait_count);
}

TEST(LinuxEventLoopTest, PreservesWaitSourceAndSystemErrorContext) {
    RuntimeFixture fixture;
    fixture.source_a.set_poll_failure(eavp::Status(
        eavp::StatusCode::kDeviceLost, "source failed", "camera", "poll", 9));
    ASSERT_TRUE(fixture.loop->initialize().ok());
    const eavp::Status source_status = fixture.loop->register_source(
        &fixture.pipeline, &fixture.source_a);
    EXPECT_EQ(eavp::StatusCode::kDeviceLost, source_status.code());
    EXPECT_EQ("camera", source_status.provider_id());

    fixture.api->saved_error = EBADF;
    fixture.api->epoll_add_result = -1;
    fixture.source_b.set_descriptors(std::vector<pollfd>(1, readable_fd(10)));
    const eavp::Status add_status = fixture.loop->register_source(
        &fixture.pipeline, &fixture.source_b);
    EXPECT_EQ(eavp::StatusCode::kIoError, add_status.code());
    EXPECT_EQ("linux_runtime", add_status.provider_id());
    EXPECT_EQ("epoll_ctl", add_status.operation());
    EXPECT_EQ(EBADF, add_status.native_code());
}

TEST(LinuxEventLoopTest, ReportsWaitReadAndWriteFailuresWithExactOperations) {
    RuntimeFixture fixture;
    ASSERT_TRUE(fixture.loop->initialize().ok());

    fixture.api->queue_error(EIO);
    eavp::Result<eavp::detail::LinuxEventLoopTurn> ready =
        fixture.loop->wait_once();
    ASSERT_FALSE(ready.ok());
    EXPECT_EQ("epoll_wait", ready.status().operation());
    EXPECT_EQ(EIO, ready.status().native_code());

    fixture.api->saved_error = EAGAIN;
    fixture.api->write_event_fd_result = -1;
    const eavp::Status write_status = fixture.loop->wake();
    EXPECT_EQ(eavp::StatusCode::kIoError, write_status.code());
    EXPECT_EQ("write(eventfd)", write_status.operation());

    fixture.api->write_event_fd_result = 0;
    fixture.api->read_event_fd_result = -1;
    fixture.api->queue_ready_fds(std::vector<int>(1, fixture.api->event_fd_result));
    ready = fixture.loop->wait_once();
    ASSERT_FALSE(ready.ok());
    EXPECT_EQ("read(eventfd)", ready.status().operation());
    EXPECT_EQ(EAGAIN, ready.status().native_code());
}

TEST(LinuxEventLoopTest, ClosesPartiallyAndFullyInitializedResourcesExactlyOnce) {
    std::vector<int> partial_closed_fds;
    FakeLinuxRuntimeApi* partial_api =
        new FakeLinuxRuntimeApi(&partial_closed_fds);
    partial_api->event_fd_result = -1;
    partial_api->saved_error = EMFILE;
    {
        eavp::detail::LinuxEventLoop partial((
            std::unique_ptr<eavp::detail::LinuxRuntimeApi>(partial_api)));
        const eavp::Status status = partial.initialize();
        EXPECT_EQ(eavp::StatusCode::kIoError, status.code());
        EXPECT_EQ("eventfd", status.operation());
        EXPECT_EQ(EMFILE, status.native_code());
        EXPECT_EQ(1, partial_api->close_count_for(40));
    }
    EXPECT_EQ(1U, partial_closed_fds.size());
    EXPECT_EQ(40, partial_closed_fds[0]);

    std::vector<int> explicitly_closed_fds;
    {
        FakeLinuxRuntimeApi* api =
            new FakeLinuxRuntimeApi(&explicitly_closed_fds);
        eavp::detail::LinuxEventLoop loop((
            std::unique_ptr<eavp::detail::LinuxRuntimeApi>(api)));
        ASSERT_TRUE(loop.initialize().ok());
        EXPECT_TRUE(loop.close().ok());
        EXPECT_TRUE(loop.close().ok());
    }
    EXPECT_EQ(2U, explicitly_closed_fds.size());

    std::vector<int> destructor_closed_fds;
    {
        FakeLinuxRuntimeApi* api =
            new FakeLinuxRuntimeApi(&destructor_closed_fds);
        eavp::detail::LinuxEventLoop loop((
            std::unique_ptr<eavp::detail::LinuxRuntimeApi>(api)));
        ASSERT_TRUE(loop.initialize().ok());
    }
    EXPECT_EQ(2U, destructor_closed_fds.size());
}

TEST(LinuxEventLoopTest, ClosesBothDescriptorsWhenControlRegistrationFails) {
    std::vector<int> closed_fds;
    FakeLinuxRuntimeApi* api = new FakeLinuxRuntimeApi(&closed_fds);
    api->epoll_add_result = -1;
    api->saved_error = EIO;
    {
        eavp::detail::LinuxEventLoop loop((
            std::unique_ptr<eavp::detail::LinuxRuntimeApi>(api)));
        const eavp::Status status = loop.initialize();
        EXPECT_EQ(eavp::StatusCode::kIoError, status.code());
        EXPECT_EQ("epoll_ctl", status.operation());
    }
    ASSERT_EQ(2U, closed_fds.size());
    EXPECT_EQ(41, closed_fds[0]);
    EXPECT_EQ(40, closed_fds[1]);
}

TEST(LinuxRuntimeApiTest, LastErrorRemainsPairedWithTheCallingThread) {
    std::unique_ptr<eavp::detail::LinuxRuntimeApi> api =
        eavp::detail::create_linux_runtime_api();
    const int epoll_fd = api->epoll_create();
    ASSERT_GE(epoll_fd, 0);

    std::mutex mutex;
    std::condition_variable condition;
    int failed_threads = 0;
    int invalid_capacity_result = 0;
    int invalid_fd_result = 0;
    int invalid_capacity_error = 0;
    int invalid_fd_error = 0;
    struct epoll_event first_event;
    struct epoll_event second_event;

    std::thread invalid_capacity([&]() {
        invalid_capacity_result =
            api->epoll_wait_events(epoll_fd, &first_event, 0, 0);
        std::unique_lock<std::mutex> lock(mutex);
        ++failed_threads;
        condition.notify_all();
        condition.wait(lock, [&]() { return failed_threads == 2; });
        invalid_capacity_error = api->last_error();
    });
    std::thread invalid_fd([&]() {
        invalid_fd_result = api->epoll_wait_events(-1, &second_event, 1, 0);
        std::unique_lock<std::mutex> lock(mutex);
        ++failed_threads;
        condition.notify_all();
        condition.wait(lock, [&]() { return failed_threads == 2; });
        invalid_fd_error = api->last_error();
    });
    invalid_capacity.join();
    invalid_fd.join();

    EXPECT_EQ(-1, invalid_capacity_result);
    EXPECT_EQ(-1, invalid_fd_result);
    EXPECT_EQ(EINVAL, invalid_capacity_error);
    EXPECT_EQ(EBADF, invalid_fd_error);
    EXPECT_EQ(0, api->close_fd(epoll_fd));
}

}  // namespace
