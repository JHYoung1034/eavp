#ifndef EAVP_PLATFORM_LINUX_V4L2_SYSTEM_HPP_
#define EAVP_PLATFORM_LINUX_V4L2_SYSTEM_HPP_

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <poll.h>
#include <sys/time.h>
#include <utility>
#include <vector>

#include "eavp/base/result.hpp"
#include "eavp/base/status.hpp"
#include "eavp/media/video_format.hpp"
#include "eavp/platform/linux/v4l2_capture.hpp"
#include "platform/linux/v4l2_api.hpp"

namespace eavp {
namespace detail {

struct V4L2NegotiatedFormat {
    V4L2NegotiatedFormat(VideoFormat&& format_value,
                         std::size_t total_capacity_value,
                         std::vector<std::size_t>&& visible_row_bytes_value,
                         std::vector<std::size_t>&& source_offsets_value)
        : format(std::move(format_value)),
          total_capacity(total_capacity_value),
          visible_row_bytes(std::move(visible_row_bytes_value)),
          source_offsets(std::move(source_offsets_value)) {}

    VideoFormat format;
    std::size_t total_capacity;
    std::vector<std::size_t> visible_row_bytes;
    std::vector<std::size_t> source_offsets;
};

struct V4L2DequeuedBuffer {
    V4L2DequeuedBuffer(std::uint32_t index_value,
                       const std::uint8_t* data_value,
                       std::size_t mapped_length_value,
                       std::uint32_t bytesused_value,
                       std::uint32_t flags_value,
                       std::uint32_t sequence_value,
                       const struct timeval& timestamp_value)
        : index(index_value), data(data_value),
          mapped_length(mapped_length_value), bytesused(bytesused_value),
          flags(flags_value), sequence(sequence_value),
          timestamp(timestamp_value) {}

    std::uint32_t index;
    const std::uint8_t* data;
    std::size_t mapped_length;
    std::uint32_t bytesused;
    std::uint32_t flags;
    std::uint32_t sequence;
    struct timeval timestamp;
};

class V4L2System {
public:
    explicit V4L2System(std::unique_ptr<V4L2Api> api);
    ~V4L2System() noexcept;
    V4L2System(V4L2System&& other) noexcept;
    V4L2System& operator=(V4L2System&& other) noexcept;
    V4L2System(const V4L2System&) = delete;
    V4L2System& operator=(const V4L2System&) = delete;

    Status prepare(const V4L2CaptureConfig& config);
    Status start();
    Status stop();
    Result<V4L2DequeuedBuffer> dequeue();
    Status requeue(std::uint32_t index);
    Status reset();
    const V4L2NegotiatedFormat& negotiated() const {
        assert(negotiated_.get() != NULL);
        return *negotiated_;
    }
    Result<std::vector<struct pollfd> > poll_descriptors() const;
    Result<bool> evaluate_poll_events(
        const std::vector<struct pollfd>& descriptors) const;

private:
    enum State {
        kCreated,
        kPrepared,
        kRunning
    };

    struct MappedRegion {
        MappedRegion(void* address_value, std::size_t length_value)
            : address(address_value), length(length_value) {}

        void* address;
        std::size_t length;
    };

    int bounded_open(const char* path, int flags);
    int bounded_ioctl(unsigned long request, void* argument);
    Status queue_buffer(std::uint32_t index);
    Status rollback(const Status& first_failure);
    void cleanup_noexcept() noexcept;

    std::unique_ptr<V4L2Api> api_;
    int fd_;
    std::vector<MappedRegion> regions_;
    bool kernel_buffers_allocated_;
    std::unique_ptr<V4L2NegotiatedFormat> negotiated_;
    State state_;
};

}  // namespace detail
}  // namespace eavp

#endif  // EAVP_PLATFORM_LINUX_V4L2_SYSTEM_HPP_
