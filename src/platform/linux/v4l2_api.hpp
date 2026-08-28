#ifndef EAVP_PLATFORM_LINUX_V4L2_API_HPP_
#define EAVP_PLATFORM_LINUX_V4L2_API_HPP_

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>

namespace eavp {
namespace detail {

class V4L2Api {
public:
    virtual ~V4L2Api() {}

    virtual int open_device(const char* path, int flags) = 0;
    virtual int device_ioctl(int fd, unsigned long request, void* argument) = 0;
    virtual void* map_memory(void* address, std::size_t length,
                             int protection, int flags, int fd,
                             std::int64_t offset) = 0;
    virtual int unmap_memory(void* address, std::size_t length) = 0;
    virtual int close_device(int fd) = 0;
    virtual int monotonic_now(struct timespec* value) = 0;
    virtual std::uint64_t maximum_mappable_offset() const = 0;
    virtual int last_error() const = 0;
};

std::unique_ptr<V4L2Api> create_linux_v4l2_api();

}  // namespace detail
}  // namespace eavp

#endif  // EAVP_PLATFORM_LINUX_V4L2_API_HPP_
