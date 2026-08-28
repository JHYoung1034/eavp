#ifndef EAVP_TESTS_SUPPORT_V4L2_TEST_UTILS_HPP_
#define EAVP_TESTS_SUPPORT_V4L2_TEST_UTILS_HPP_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <poll.h>
#include <set>
#include <vector>

#include "eavp/base/status.hpp"
#include "eavp/media/frame.hpp"
#include "eavp/media/node.hpp"
#include "eavp/media/port.hpp"
#include "eavp/media/video_format.hpp"
#include "eavp/platform/linux/v4l2_capture.hpp"

namespace eavp_test {

struct V4L2ConfigCase {
    const char* name;
    const char* device_path;
    eavp::PixelFormat pixel_format;
    int width;
    int height;
    int frame_rate_numerator;
    int frame_rate_denominator;
    std::size_t buffer_count;
    eavp::StatusCode expected_status;
};

class RuntimeV4L2WaitSource : public eavp::LinuxWaitSource {
public:
    explicit RuntimeV4L2WaitSource(eavp::V4L2SourceNode* source)
        : source_(source) {}

    eavp::Result<std::vector<struct pollfd> > poll_descriptors() override {
        eavp::Result<std::vector<struct pollfd> > result =
            source_->poll_descriptors();
        if (!result.ok()) return result;
        std::vector<struct pollfd> descriptors = result.take_value();
        for (std::size_t index = 0U; index < descriptors.size(); ++index) {
            descriptors[index].events = static_cast<short>(
                descriptors[index].events &
                static_cast<short>(POLLIN | POLLOUT | POLLPRI));
        }
        return eavp::Result<std::vector<struct pollfd> >(descriptors);
    }

    eavp::Result<bool> evaluate_poll_events(
        const std::vector<struct pollfd>& descriptors) override {
        return source_->evaluate_poll_events(descriptors);
    }

private:
    eavp::V4L2SourceNode* source_;
};

class FrameChecksumSink : public eavp::MediaNode {
public:
    FrameChecksumSink()
        : eavp::MediaNode("frame-checksum"),
          input_("video-input", 4U, eavp::OverflowPolicy::kDropOldest),
          frame_count_(0U), checksum_(14695981039346656037ULL),
          pts_monotonic_(true), has_pts_(false), last_pts_(0),
          duplicate_sequence_(false), sequences_(), layouts_(),
          layouts_consistent_(true) {}

    eavp::InputPort<eavp::VideoFrame>& input() { return input_; }

    bool wait_for_frames(std::size_t count, int timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(
            lock, std::chrono::milliseconds(timeout_ms),
            [this, count]() { return frame_count_ >= count; });
    }

    std::size_t frame_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return frame_count_;
    }

    std::uint64_t checksum() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return checksum_;
    }

    bool pts_are_monotonic() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pts_monotonic_;
    }

    bool has_duplicate_sequence() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return duplicate_sequence_;
    }

    bool plane_layouts_are_consistent() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return layouts_consistent_;
    }

    std::vector<eavp::PlaneLayout> plane_layouts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return layouts_;
    }

    std::size_t dropped_count() const { return input_.dropped_count(); }

protected:
    eavp::Status on_tick() override {
        eavp::Result<std::shared_ptr<const eavp::VideoFrame> > received =
            input_.receive();
        if (!received.ok()) return received.status();
        const std::shared_ptr<const eavp::VideoFrame> frame =
            received.take_value();

        std::vector<eavp::PlaneLayout> layouts;
        std::vector<std::uint8_t> bytes;
        for (std::size_t plane = 0U;
             plane < frame->buffer().plane_count(); ++plane) {
            eavp::Result<eavp::PlaneLayout> layout =
                frame->buffer().plane_layout(plane);
            if (!layout.ok()) return layout.status();
            layouts.push_back(layout.value());
            eavp::Result<eavp::MappedRegion> mapped =
                frame->buffer().map_plane(plane, eavp::MapMode::kReadOnly);
            if (!mapped.ok()) return mapped.status();
            eavp::MappedRegion region = mapped.take_value();
            bytes.insert(bytes.end(), region.data(),
                         region.data() + region.size());
        }
        if (bytes.size() < 4U) {
            return eavp::Status(eavp::StatusCode::kCorruptData);
        }
        const std::uint32_t sequence =
            static_cast<std::uint32_t>(bytes[0]) |
            (static_cast<std::uint32_t>(bytes[1]) << 8U) |
            (static_cast<std::uint32_t>(bytes[2]) << 16U) |
            (static_cast<std::uint32_t>(bytes[3]) << 24U);

        std::lock_guard<std::mutex> lock(mutex_);
        if (has_pts_ && frame->pts() <= last_pts_) pts_monotonic_ = false;
        has_pts_ = true;
        last_pts_ = frame->pts();
        if (!sequences_.insert(sequence).second) duplicate_sequence_ = true;
        remember_layouts(layouts);
        for (std::size_t index = 0U; index < bytes.size(); ++index) {
            checksum_ ^= static_cast<std::uint64_t>(bytes[index]);
            checksum_ *= 1099511628211ULL;
        }
        ++frame_count_;
        condition_.notify_all();
        return eavp::Status::ok_status();
    }

private:
    void remember_layouts(const std::vector<eavp::PlaneLayout>& layouts) {
        if (layouts_.empty()) {
            layouts_ = layouts;
            return;
        }
        if (layouts_.size() != layouts.size()) {
            layouts_consistent_ = false;
            return;
        }
        for (std::size_t index = 0U; index < layouts.size(); ++index) {
            if (layouts_[index].offset != layouts[index].offset ||
                layouts_[index].size != layouts[index].size ||
                layouts_[index].stride != layouts[index].stride) {
                layouts_consistent_ = false;
            }
        }
    }

    eavp::InputPort<eavp::VideoFrame> input_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t frame_count_;
    std::uint64_t checksum_;
    bool pts_monotonic_;
    bool has_pts_;
    std::int64_t last_pts_;
    bool duplicate_sequence_;
    std::set<std::uint32_t> sequences_;
    std::vector<eavp::PlaneLayout> layouts_;
    bool layouts_consistent_;
};

}  // namespace eavp_test

#endif  // EAVP_TESTS_SUPPORT_V4L2_TEST_UTILS_HPP_
