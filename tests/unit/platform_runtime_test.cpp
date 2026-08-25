#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "eavp/platform/linux/platform_runtime.hpp"
#include "eavp/platform/linux/wait_source.hpp"

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

}  // namespace
