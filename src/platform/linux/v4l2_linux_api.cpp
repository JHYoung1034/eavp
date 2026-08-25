#include "platform/linux/v4l2_api.hpp"

#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

namespace eavp {
namespace detail {
namespace {

class LinuxV4L2Api : public V4L2Api {
public:
    LinuxV4L2Api() : last_error_(0) {}

    int open_device(const char* path, int flags) override {
        const int result = ::open(path, flags);
        remember_error(result < 0);
        return result;
    }

    int device_ioctl(int fd, unsigned long request, void* argument) override {
        const int result = ::ioctl(fd, request, argument);
        remember_error(result < 0);
        return result;
    }

    void* map_memory(void* address, std::size_t length, int protection,
                     int flags, int fd, std::int64_t offset) override {
        if (offset < 0 ||
            static_cast<std::uint64_t>(offset) >
                static_cast<std::uint64_t>(
                    std::numeric_limits<off_t>::max())) {
            last_error_ = EOVERFLOW;
            return MAP_FAILED;
        }
        void* const result = ::mmap(address, length, protection, flags, fd,
                                    static_cast<off_t>(offset));
        remember_error(result == MAP_FAILED);
        return result;
    }

    int unmap_memory(void* address, std::size_t length) override {
        const int result = ::munmap(address, length);
        remember_error(result < 0);
        return result;
    }

    int close_device(int fd) override {
        const int result = ::close(fd);
        remember_error(result < 0);
        return result;
    }

    int monotonic_now(struct timespec* value) override {
        const int result = ::clock_gettime(CLOCK_MONOTONIC, value);
        remember_error(result < 0);
        return result;
    }

    std::uint64_t maximum_mappable_offset() const override {
        return static_cast<std::uint64_t>(
            std::numeric_limits<off_t>::max());
    }

    int last_error() const override { return last_error_; }

private:
    void remember_error(bool failed) {
        last_error_ = failed ? errno : 0;
    }

    int last_error_;
};

}  // namespace

std::unique_ptr<V4L2Api> create_linux_v4l2_api() {
    return std::unique_ptr<V4L2Api>(new LinuxV4L2Api());
}

}  // namespace detail
}  // namespace eavp
