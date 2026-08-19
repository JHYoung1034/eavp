#ifndef EAVP_MEDIA_BUFFER_HPP_
#define EAVP_MEDIA_BUFFER_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "eavp/base/result.hpp"

namespace eavp {

enum class MemoryDomain {
    kCpu,
    kMmap,
    kDmaBuf,
    kDeviceOpaque,
};

enum class MapMode {
    kReadOnly,
    kReadWrite,
};

struct PlaneLayout {
    PlaneLayout(std::size_t offset_value, std::size_t size_value, std::size_t stride_value)
        : offset(offset_value), size(size_value), stride(stride_value) {}

    std::size_t offset;
    std::size_t size;
    std::size_t stride;
};

class NativeBufferHandle {
public:
    explicit NativeBufferHandle(int file_descriptor);
    ~NativeBufferHandle();

    NativeBufferHandle(NativeBufferHandle&& other) noexcept;
    NativeBufferHandle& operator=(NativeBufferHandle&& other) noexcept;
    NativeBufferHandle(const NativeBufferHandle&) = delete;
    NativeBufferHandle& operator=(const NativeBufferHandle&) = delete;

    int file_descriptor() const { return file_descriptor_; }

private:
    void close();

    int file_descriptor_;
};

class BufferStorage {
public:
    virtual ~BufferStorage() {}

    virtual MemoryDomain memory_domain() const = 0;
    virtual std::size_t capacity() const = 0;
    virtual const std::string& provider_id() const = 0;
    virtual Status map(MapMode mode, std::uint8_t** data, std::size_t* size) = 0;
    virtual Status unmap() = 0;
    virtual Result<NativeBufferHandle> export_dmabuf() const = 0;
};

class MappedRegion {
public:
    ~MappedRegion();

    MappedRegion(MappedRegion&& other) noexcept;
    MappedRegion& operator=(MappedRegion&& other) noexcept;
    MappedRegion(const MappedRegion&) = delete;
    MappedRegion& operator=(const MappedRegion&) = delete;

    const std::uint8_t* data() const { return data_; }
    std::uint8_t* mutable_data() { return data_; }
    std::size_t size() const { return size_; }

private:
    friend class Buffer;

    MappedRegion(const std::shared_ptr<BufferStorage>& storage, std::uint8_t* data,
                 std::size_t size);
    void unmap();

    std::shared_ptr<BufferStorage> storage_;
    std::uint8_t* data_;
    std::size_t size_;
};

class Buffer {
public:
    static Result<Buffer> allocate(std::size_t size);
    static Result<Buffer> create(const std::shared_ptr<BufferStorage>& storage,
                                 const std::vector<PlaneLayout>& planes);

    Result<MappedRegion> map_plane(std::size_t plane_index, MapMode mode) const;
    Result<NativeBufferHandle> export_dmabuf() const;
    Result<Buffer> slice(std::size_t offset, std::size_t length) const;
    Result<Buffer> slice_plane(std::size_t plane_index, std::size_t offset,
                               std::size_t length) const;

    MemoryDomain memory_domain() const { return storage_->memory_domain(); }
    std::size_t plane_count() const { return planes_.size(); }
    Result<PlaneLayout> plane_layout(std::size_t plane_index) const {
        if (plane_index >= planes_.size()) {
            return Result<PlaneLayout>(
                Status(StatusCode::kInvalidArgument, "plane index is invalid"));
        }
        return Result<PlaneLayout>(planes_[plane_index]);
    }

private:
    Buffer(const std::shared_ptr<BufferStorage>& storage, const std::vector<PlaneLayout>& planes)
        : storage_(storage), planes_(planes) {}

    std::shared_ptr<BufferStorage> storage_;
    std::vector<PlaneLayout> planes_;
};

}  // namespace eavp

#endif  // EAVP_MEDIA_BUFFER_HPP_
