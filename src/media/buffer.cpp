#include "eavp/media/buffer.hpp"

#include <new>
#include <utility>
#include <vector>

#include <unistd.h>

namespace eavp {

namespace {

class CpuBufferStorage : public BufferStorage {
public:
    explicit CpuBufferStorage(std::size_t size) : bytes_(size), provider_id_("cpu") {}

    MemoryDomain memory_domain() const override { return MemoryDomain::kCpu; }
    std::size_t capacity() const override { return bytes_.size(); }
    const std::string& provider_id() const override { return provider_id_; }

    Status map(MapMode, std::uint8_t** data, std::size_t* size) override {
        *data = bytes_.data();
        *size = bytes_.size();
        return Status::ok_status();
    }

    Status unmap() override { return Status::ok_status(); }

    Result<NativeBufferHandle> export_dmabuf() const override {
        return Result<NativeBufferHandle>(
            Status(StatusCode::kUnsupported, "CPU storage cannot export a DMABUF handle"));
    }

private:
    std::vector<std::uint8_t> bytes_;
    std::string provider_id_;
};

bool is_within(std::size_t offset, std::size_t size, std::size_t capacity) {
    return offset <= capacity && size <= capacity - offset;
}

std::size_t greatest_common_divisor(std::size_t left, std::size_t right) {
    while (right != 0U) {
        const std::size_t remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}

Status validate_planes(const std::shared_ptr<BufferStorage>& storage,
                       const std::vector<PlaneLayout>& planes) {
    if (!storage) {
        return Status(StatusCode::kInvalidArgument, "buffer storage must not be null");
    }
    if (planes.empty()) {
        return Status(StatusCode::kInvalidArgument, "buffer must contain at least one plane");
    }
    if (storage->address_alignment() == 0U) {
        return Status(StatusCode::kInvalidArgument,
                      "buffer storage address alignment must be positive");
    }

    const std::size_t capacity = storage->capacity();
    for (std::size_t index = 0; index < planes.size(); ++index) {
        const PlaneLayout& plane = planes[index];
        if (plane.stride == 0U) {
            return Status(StatusCode::kInvalidArgument, "plane stride must be positive");
        }
        if (!is_within(plane.offset, plane.size, capacity)) {
            return Status(StatusCode::kInvalidArgument, "plane is outside storage capacity");
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            const PlaneLayout& other = planes[previous];
            const std::size_t plane_end = plane.offset + plane.size;
            const std::size_t other_end = other.offset + other.size;
            if (plane.offset < other_end && other.offset < plane_end) {
                return Status(StatusCode::kInvalidArgument, "buffer planes must not overlap");
            }
        }
    }
    return Status::ok_status();
}

}  // namespace

Result<std::string> memory_domain_name(MemoryDomain memory_domain) noexcept {
    try {
        switch (memory_domain) {
            case MemoryDomain::kCpu:
                return Result<std::string>(std::string("cpu"));
            case MemoryDomain::kMmap:
                return Result<std::string>(std::string("mmap"));
            case MemoryDomain::kDmaBuf:
                return Result<std::string>(std::string("dmabuf"));
            case MemoryDomain::kDeviceOpaque:
                return Result<std::string>(std::string("device_opaque"));
        }
        return Result<std::string>(Status(
            StatusCode::kInvalidArgument, "memory domain has no stable state name"));
    } catch (const std::bad_alloc&) {
        return Result<std::string>(Status(StatusCode::kResourceExhausted));
    } catch (...) {
        return Result<std::string>(Status(StatusCode::kInternal));
    }
}

NativeBufferHandle::NativeBufferHandle(int file_descriptor) : file_descriptor_(file_descriptor) {}

NativeBufferHandle::~NativeBufferHandle() {
    close();
}

NativeBufferHandle::NativeBufferHandle(NativeBufferHandle&& other) noexcept
    : file_descriptor_(other.file_descriptor_) {
    other.file_descriptor_ = -1;
}

NativeBufferHandle& NativeBufferHandle::operator=(NativeBufferHandle&& other) noexcept {
    if (this != &other) {
        close();
        file_descriptor_ = other.file_descriptor_;
        other.file_descriptor_ = -1;
    }
    return *this;
}

void NativeBufferHandle::close() {
    if (file_descriptor_ >= 0) {
        ::close(file_descriptor_);
        file_descriptor_ = -1;
    }
}

MappedRegion::MappedRegion(const std::shared_ptr<BufferStorage>& storage, std::uint8_t* data,
                           std::size_t size, MapMode mode)
    : storage_(storage), data_(data), size_(size), mode_(mode) {}

MappedRegion::~MappedRegion() {
    unmap();
}

MappedRegion::MappedRegion(MappedRegion&& other) noexcept
    : storage_(std::move(other.storage_)), data_(other.data_), size_(other.size_),
      mode_(other.mode_) {
    other.data_ = NULL;
    other.size_ = 0U;
}

MappedRegion& MappedRegion::operator=(MappedRegion&& other) noexcept {
    if (this != &other) {
        unmap();
        storage_ = std::move(other.storage_);
        data_ = other.data_;
        size_ = other.size_;
        mode_ = other.mode_;
        other.data_ = NULL;
        other.size_ = 0U;
    }
    return *this;
}

void MappedRegion::unmap() {
    if (storage_) {
        storage_->unmap();
        storage_.reset();
    }
    data_ = NULL;
    size_ = 0U;
}

Result<Buffer> Buffer::allocate(std::size_t size) {
    if (size == 0U) {
        return Result<Buffer>(
            Status(StatusCode::kInvalidArgument, "buffer size must be positive"));
    }

    try {
        std::shared_ptr<BufferStorage> storage(new CpuBufferStorage(size));
        std::vector<PlaneLayout> planes;
        planes.push_back(PlaneLayout(0U, size, size));
        return create(storage, planes);
    } catch (const std::bad_alloc&) {
        return Result<Buffer>(
            Status(StatusCode::kResourceExhausted, "failed to allocate CPU buffer storage"));
    }
}

Result<Buffer> Buffer::create(const std::shared_ptr<BufferStorage>& storage,
                              const std::vector<PlaneLayout>& planes) {
    const Status validation = validate_planes(storage, planes);
    if (!validation.ok()) {
        return Result<Buffer>(validation);
    }

    try {
        return Result<Buffer>(Buffer(storage, planes));
    } catch (const std::bad_alloc&) {
        return Result<Buffer>(
            Status(StatusCode::kResourceExhausted, "failed to create buffer metadata"));
    }
}

Result<MappedRegion> Buffer::map_plane(std::size_t plane_index, MapMode mode) const {
    if (plane_index >= planes_.size()) {
        return Result<MappedRegion>(Status(StatusCode::kInvalidArgument, "plane index is invalid"));
    }

    std::uint8_t* mapped_data = NULL;
    std::size_t mapped_size = 0U;
    const Status map_status = storage_->map(mode, &mapped_data, &mapped_size);
    if (!map_status.ok()) {
        return Result<MappedRegion>(map_status);
    }

    const PlaneLayout& plane = planes_[plane_index];
    if (!is_within(plane.offset, plane.size, mapped_size) ||
        (mapped_data == NULL && plane.size != 0U)) {
        storage_->unmap();
        return Result<MappedRegion>(
            Status(StatusCode::kInvalidArgument, "mapped storage cannot cover requested plane"));
    }
    std::uint8_t* plane_data = mapped_data == NULL ? NULL : mapped_data + plane.offset;
    return Result<MappedRegion>(MappedRegion(storage_, plane_data, plane.size, mode));
}

Result<NativeBufferHandle> Buffer::export_dmabuf() const {
    return storage_->export_dmabuf();
}

Result<Buffer> Buffer::slice(std::size_t offset, std::size_t length) const {
    if (planes_.size() != 1U) {
        return Result<Buffer>(
            Status(StatusCode::kUnsupported, "slice requires a single-plane buffer"));
    }
    return slice_plane(0U, offset, length);
}

Result<Buffer> Buffer::slice_plane(std::size_t plane_index, std::size_t offset,
                                   std::size_t length) const {
    if (plane_index >= planes_.size()) {
        return Result<Buffer>(Status(StatusCode::kInvalidArgument, "plane index is invalid"));
    }

    const PlaneLayout& plane = planes_[plane_index];
    if (!is_within(offset, length, plane.size)) {
        return Result<Buffer>(
            Status(StatusCode::kInvalidArgument, "buffer slice is out of range"));
    }

    std::vector<PlaneLayout> sliced_planes;
    try {
        sliced_planes.push_back(PlaneLayout(plane.offset + offset, length, plane.stride));
    } catch (const std::bad_alloc&) {
        return Result<Buffer>(
            Status(StatusCode::kResourceExhausted, "failed to create slice metadata"));
    }
    return create(storage_, sliced_planes);
}

Result<std::size_t> Buffer::plane_address_alignment(
    std::size_t plane_index) const {
    if (plane_index >= planes_.size()) {
        return Result<std::size_t>(
            Status(StatusCode::kInvalidArgument, "plane index is invalid"));
    }
    const std::size_t storage_alignment = storage_->address_alignment();
    if (storage_alignment == 0U) {
        return Result<std::size_t>(Status(
            StatusCode::kInvalidState,
            "buffer storage has an invalid address alignment guarantee"));
    }
    const std::size_t offset = planes_[plane_index].offset;
    return Result<std::size_t>(offset == 0U
                                   ? storage_alignment
                                   : greatest_common_divisor(storage_alignment, offset));
}

}  // namespace eavp
