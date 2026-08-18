#ifndef EAVP_MEDIA_BUFFER_HPP_
#define EAVP_MEDIA_BUFFER_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "eavp/base/result.hpp"

namespace eavp {

enum class MemoryType {
    kCpu,
    kMmap,
    kDma,
    kDmaBuf,
    kGpu,
    kNpu,
    kHardwareNative,
};

class Buffer {
public:
    static Result<Buffer> allocate(std::size_t size) {
        if (size == 0U) {
            return Result<Buffer>(
                Status(StatusCode::kInvalidArgument, "buffer size must be positive"));
        }
        return Result<Buffer>(Buffer(std::shared_ptr<Storage>(new Storage(size)), 0U, size));
    }

    Result<Buffer> slice(std::size_t offset, std::size_t length) const {
        if (offset > size_ || length > size_ - offset) {
            return Result<Buffer>(
                Status(StatusCode::kInvalidArgument, "buffer slice is out of range"));
        }
        return Result<Buffer>(Buffer(storage_, offset_ + offset, length));
    }

    std::uint8_t* mutable_data() { return storage_->bytes.data() + offset_; }
    const std::uint8_t* data() const { return storage_->bytes.data() + offset_; }
    std::size_t size() const { return size_; }
    std::size_t capacity() const { return storage_->bytes.size() - offset_; }
    MemoryType memory_type() const { return MemoryType::kCpu; }
    int file_descriptor() const { return -1; }

private:
    struct Storage {
        explicit Storage(std::size_t size) : bytes(size) {}
        std::vector<std::uint8_t> bytes;
    };

    Buffer(const std::shared_ptr<Storage>& storage, std::size_t offset, std::size_t size)
        : storage_(storage), offset_(offset), size_(size) {}

    std::shared_ptr<Storage> storage_;
    std::size_t offset_;
    std::size_t size_;
};

}  // namespace eavp

#endif  // EAVP_MEDIA_BUFFER_HPP_

