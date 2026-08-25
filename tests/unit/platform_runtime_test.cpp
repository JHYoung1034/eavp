#include <gtest/gtest.h>

#include <cerrno>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <sys/epoll.h>
#include <thread>
#include <vector>

#include "eavp/platform/linux/platform_runtime.hpp"
#include "eavp/platform/linux/wait_source.hpp"
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

TEST(LinuxPlatformRuntimeTest, CreatesTheCreatedStateButDefersRuntimeOperations) {
    const eavp::LinuxPlatformRuntimeConfig config =
        eavp::LinuxPlatformRuntimeConfig::create(1, 2000).take_value();
    eavp::Result<std::unique_ptr<eavp::LinuxPlatformRuntime> > created =
        eavp::LinuxPlatformRuntime::create(config, NULL);

    ASSERT_TRUE(created.ok());
    const std::unique_ptr<eavp::LinuxPlatformRuntime> runtime = created.take_value();
    EXPECT_EQ(eavp::PlatformRuntimeState::kCreated, runtime->state());
    EXPECT_EQ(eavp::StatusCode::kInvalidState, runtime->last_failure().code());
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              runtime->register_pipeline(NULL, std::vector<eavp::LinuxWaitSource*>()).code());
    EXPECT_EQ(eavp::StatusCode::kInvalidState, runtime->start().code());
    EXPECT_EQ(eavp::StatusCode::kInvalidState, runtime->stop().code());
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

TEST(LinuxEventLoopTest, RegistersLevelTriggeredPollInterestsExactly) {
    RuntimeFixture fixture;
    const struct pollfd input = {10, static_cast<short>(POLLIN), 0};
    const struct pollfd output_priority = {
        11, static_cast<short>(POLLOUT | POLLPRI), 0};
    fixture.source_a.set_descriptors(
        std::vector<pollfd>{input, output_priority});

    ASSERT_TRUE(fixture.loop->initialize().ok());
    ASSERT_TRUE(fixture.loop->register_source(
        &fixture.pipeline, &fixture.source_a).ok());

    ASSERT_EQ(3U, fixture.api->add_calls.size());
    EXPECT_EQ(EPOLLIN, fixture.api->add_calls[0].events);
    EXPECT_EQ(EPOLLIN, fixture.api->add_calls[1].events);
    EXPECT_EQ(static_cast<std::uint32_t>(EPOLLOUT | EPOLLPRI),
              fixture.api->add_calls[2].events);
    EXPECT_EQ(0U, fixture.api->add_calls[1].events &
                      static_cast<std::uint32_t>(EPOLLET | EPOLLONESHOT));
    EXPECT_EQ(0U, fixture.api->add_calls[2].events &
                      static_cast<std::uint32_t>(EPOLLET | EPOLLONESHOT));
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
