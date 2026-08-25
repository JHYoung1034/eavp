#ifndef EAVP_PLATFORM_LINUX_V4L2_CAPTURE_INTERNAL_HPP_
#define EAVP_PLATFORM_LINUX_V4L2_CAPTURE_INTERNAL_HPP_

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "eavp/media/buffer.hpp"
#include "eavp/platform/linux/v4l2_capture.hpp"
#include "platform/linux/v4l2_system.hpp"

namespace eavp {
namespace detail {

class V4L2FrameAllocator {
public:
    virtual ~V4L2FrameAllocator() {}
    virtual Result<Buffer> allocate(
        std::size_t capacity, const std::vector<PlaneLayout>& planes) = 0;
};

class V4L2FormatFactory {
public:
    virtual ~V4L2FormatFactory() {}
    virtual Result<VideoFormat> create_cpu(
        const VideoFormat& negotiated) = 0;
    virtual Result<VideoFormat> clone_actual(
        const VideoFormat& negotiated) = 0;
};

class V4L2Clock {
public:
    virtual ~V4L2Clock() {}
    virtual int monotonic_now(struct timespec* value) = 0;
    virtual int last_error() const = 0;
};

class V4L2Observer {
public:
    virtual ~V4L2Observer() {}
    virtual Status on_captured(std::size_t copied_bytes) = 0;
    virtual Status on_would_block() = 0;
    virtual Status on_pending(bool pending) = 0;
    virtual Status on_dropped_on_stop() = 0;
    virtual Status on_sequence_gap(std::uint32_t missing_frames) = 0;
    virtual Status on_fatal(const Status& failure) = 0;
};

class V4L2SourceNodeTestPeer {
public:
    static Result<std::unique_ptr<V4L2SourceNode> > create(
        const std::string& id, const V4L2CaptureConfig& config,
        MetricRegistry* metrics, std::unique_ptr<V4L2System> system,
        std::unique_ptr<V4L2FrameAllocator> allocator,
        // observer 仅由测试调用方持有，必须在 Node 销毁后再销毁。
        V4L2Observer* observer = NULL,
        std::unique_ptr<V4L2FormatFactory> format_factory =
            std::unique_ptr<V4L2FormatFactory>(),
        std::unique_ptr<V4L2Clock> clock =
            std::unique_ptr<V4L2Clock>());
};

}  // namespace detail
}  // namespace eavp

#endif  // EAVP_PLATFORM_LINUX_V4L2_CAPTURE_INTERNAL_HPP_
