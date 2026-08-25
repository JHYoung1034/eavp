#ifndef EAVP_TESTS_SUPPORT_RUNTIME_TEST_UTILS_HPP_
#define EAVP_TESTS_SUPPORT_RUNTIME_TEST_UTILS_HPP_

#include <memory>
#include <vector>

#include "eavp/media/pipeline.hpp"
#include "eavp/platform/linux/wait_source.hpp"
#include "../../src/platform/linux/linux_event_loop.hpp"
#include "fake_linux_runtime_api.hpp"

namespace eavp_test {

inline struct pollfd readable_fd(int fd) {
    const struct pollfd descriptor = {fd, static_cast<short>(POLLIN), 0};
    return descriptor;
}

class FakeWaitSource : public eavp::LinuxWaitSource {
public:
    FakeWaitSource()
        : descriptors_(), evaluated_descriptors_(), evaluate_result_(true),
          poll_failure_(), evaluate_failure_(), poll_call_count_(0),
          evaluate_call_count_(0) {}

    explicit FakeWaitSource(const std::vector<struct pollfd>& descriptors)
        : descriptors_(descriptors), evaluated_descriptors_(),
          evaluate_result_(true), poll_failure_(), evaluate_failure_(),
          poll_call_count_(0), evaluate_call_count_(0) {}

    eavp::Result<std::vector<struct pollfd> > poll_descriptors() {
        ++poll_call_count_;
        if (!poll_failure_.ok()) {
            return eavp::Result<std::vector<struct pollfd> >(poll_failure_);
        }
        return eavp::Result<std::vector<struct pollfd> >(descriptors_);
    }

    eavp::Result<bool> evaluate_poll_events(
        const std::vector<struct pollfd>& descriptors) {
        ++evaluate_call_count_;
        evaluated_descriptors_ = descriptors;
        if (!evaluate_failure_.ok()) {
            return eavp::Result<bool>(evaluate_failure_);
        }
        return eavp::Result<bool>(evaluate_result_);
    }

    void set_descriptors(const std::vector<struct pollfd>& descriptors) {
        descriptors_ = descriptors;
    }
    void set_evaluate_result(bool value) { evaluate_result_ = value; }
    void set_poll_failure(const eavp::Status& status) { poll_failure_ = status; }
    void set_evaluate_failure(const eavp::Status& status) {
        evaluate_failure_ = status;
    }
    const std::vector<struct pollfd>& evaluated_descriptors() const {
        return evaluated_descriptors_;
    }
    int poll_call_count() const { return poll_call_count_; }
    int evaluate_call_count() const { return evaluate_call_count_; }

private:
    std::vector<struct pollfd> descriptors_;
    std::vector<struct pollfd> evaluated_descriptors_;
    bool evaluate_result_;
    eavp::Status poll_failure_;
    eavp::Status evaluate_failure_;
    int poll_call_count_;
    int evaluate_call_count_;
};

struct RuntimeFixture {
    RuntimeFixture()
        : pipeline("runtime-fixture"), source_a(), source_b(),
          api(new FakeLinuxRuntimeApi()),
          loop(new eavp::detail::LinuxEventLoop(
              std::unique_ptr<eavp::detail::LinuxRuntimeApi>(api))) {}

    eavp::MediaPipeline pipeline;
    FakeWaitSource source_a;
    FakeWaitSource source_b;
    FakeLinuxRuntimeApi* api;
    std::unique_ptr<eavp::detail::LinuxEventLoop> loop;
};

}  // namespace eavp_test

#endif  // EAVP_TESTS_SUPPORT_RUNTIME_TEST_UTILS_HPP_
