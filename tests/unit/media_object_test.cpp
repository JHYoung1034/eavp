#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "eavp/media/buffer.hpp"
#include "eavp/media/frame.hpp"
#include "eavp/media/media_packet.hpp"
#include "eavp/media/video_codec.hpp"
#include "eavp/media/video_format.hpp"

namespace {

class UnmappableStorage : public eavp::BufferStorage {
public:
    explicit UnmappableStorage(
        std::size_t capacity,
        eavp::MemoryDomain memory_domain = eavp::MemoryDomain::kDeviceOpaque)
        : capacity_(capacity), memory_domain_(memory_domain), provider_id_("test") {}

    eavp::MemoryDomain memory_domain() const override {
        return memory_domain_;
    }
    std::size_t capacity() const override { return capacity_; }
    const std::string& provider_id() const override { return provider_id_; }
    eavp::Status map(eavp::MapMode, std::uint8_t**, std::size_t*) override {
        return eavp::Status(eavp::StatusCode::kUnsupported, "test storage cannot map");
    }
    eavp::Status unmap() override { return eavp::Status::ok_status(); }
    eavp::Result<eavp::NativeBufferHandle> export_dmabuf() const override {
        return eavp::Result<eavp::NativeBufferHandle>(
            eavp::Status(eavp::StatusCode::kUnsupported, "test storage cannot export"));
    }

private:
    std::size_t capacity_;
    eavp::MemoryDomain memory_domain_;
    std::string provider_id_;
};

class ShortMappingStorage : public eavp::BufferStorage {
public:
    ShortMappingStorage() : bytes_(8U), unmap_count_(0), provider_id_("test") {}

    eavp::MemoryDomain memory_domain() const override { return eavp::MemoryDomain::kMmap; }
    std::size_t capacity() const override { return bytes_.size(); }
    const std::string& provider_id() const override { return provider_id_; }
    eavp::Status map(eavp::MapMode, std::uint8_t** data, std::size_t* size) override {
        *data = bytes_.data();
        *size = 4U;
        return eavp::Status::ok_status();
    }
    eavp::Status unmap() override {
        ++unmap_count_;
        return eavp::Status::ok_status();
    }
    eavp::Result<eavp::NativeBufferHandle> export_dmabuf() const override {
        return eavp::Result<eavp::NativeBufferHandle>(
            eavp::Status(eavp::StatusCode::kUnsupported, "test storage cannot export"));
    }
    int unmap_count() const { return unmap_count_; }

private:
    std::vector<std::uint8_t> bytes_;
    int unmap_count_;
    std::string provider_id_;
};

class CountingMappingStorage : public eavp::BufferStorage {
public:
    CountingMappingStorage() : bytes_(8U), unmap_count_(0), provider_id_("test") {}

    eavp::MemoryDomain memory_domain() const override { return eavp::MemoryDomain::kMmap; }
    std::size_t capacity() const override { return bytes_.size(); }
    const std::string& provider_id() const override { return provider_id_; }
    eavp::Status map(eavp::MapMode, std::uint8_t** data, std::size_t* size) override {
        *data = bytes_.data();
        *size = bytes_.size();
        return eavp::Status::ok_status();
    }
    eavp::Status unmap() override {
        ++unmap_count_;
        return eavp::Status::ok_status();
    }
    eavp::Result<eavp::NativeBufferHandle> export_dmabuf() const override {
        return eavp::Result<eavp::NativeBufferHandle>(
            eavp::Status(eavp::StatusCode::kUnsupported, "test storage cannot export"));
    }
    int unmap_count() const { return unmap_count_; }

private:
    std::vector<std::uint8_t> bytes_;
    int unmap_count_;
    std::string provider_id_;
};

class DmaBufStorage : public eavp::BufferStorage {
public:
    explicit DmaBufStorage(int file_descriptor) : file_descriptor_(file_descriptor), provider_id_("test") {}

    eavp::MemoryDomain memory_domain() const override { return eavp::MemoryDomain::kDmaBuf; }
    std::size_t capacity() const override { return 1U; }
    const std::string& provider_id() const override { return provider_id_; }
    eavp::Status map(eavp::MapMode, std::uint8_t**, std::size_t*) override {
        return eavp::Status(eavp::StatusCode::kUnsupported, "test storage cannot map");
    }
    eavp::Status unmap() override { return eavp::Status::ok_status(); }
    eavp::Result<eavp::NativeBufferHandle> export_dmabuf() const override {
        const int duplicate = dup(file_descriptor_);
        if (duplicate < 0) {
            return eavp::Result<eavp::NativeBufferHandle>(
                eavp::Status(eavp::StatusCode::kIoError, "failed to duplicate fd"));
        }
        return eavp::Result<eavp::NativeBufferHandle>(eavp::NativeBufferHandle(duplicate));
    }

private:
    int file_descriptor_;
    std::string provider_id_;
};

TEST(BufferTest, CpuPlaneMappingSharesStorageAndUnmapsOnScopeExit) {
    eavp::Buffer buffer = eavp::Buffer::allocate(8U).take_value();
    {
        eavp::MappedRegion mapped =
            buffer.map_plane(0U, eavp::MapMode::kReadWrite).take_value();
        ASSERT_EQ(8U, mapped.size());
        mapped.mutable_data()[3] = 0x5a;
    }
    const eavp::Buffer copy = buffer;
    eavp::MappedRegion mapped =
        copy.map_plane(0U, eavp::MapMode::kReadOnly).take_value();
    EXPECT_EQ(0x5a, mapped.data()[3]);
    EXPECT_EQ(eavp::MemoryDomain::kCpu, copy.memory_domain());
}

TEST(BufferTest, CpuStorageAllowsReentrantMappingsOfTheSamePlane) {
    eavp::Buffer buffer = eavp::Buffer::allocate(8U).take_value();
    eavp::MappedRegion writable =
        buffer.map_plane(0U, eavp::MapMode::kReadWrite).take_value();
    eavp::MappedRegion readable =
        buffer.map_plane(0U, eavp::MapMode::kReadOnly).take_value();

    writable.mutable_data()[2] = 0x7cU;

    EXPECT_EQ(0x7cU, readable.data()[2]);
    EXPECT_EQ(static_cast<std::uint8_t*>(NULL), readable.mutable_data());
}

TEST(BufferTest, RejectsPlaneBeyondStorageAndReportsUnmappableDeviceMemory) {
    std::shared_ptr<eavp::BufferStorage> storage(new UnmappableStorage(64U));
    std::vector<eavp::PlaneLayout> invalid;
    invalid.push_back(eavp::PlaneLayout(48U, 32U, 16U));
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::Buffer::create(storage, invalid).status().code());

    std::vector<eavp::PlaneLayout> valid;
    valid.push_back(eavp::PlaneLayout(0U, 64U, 16U));
    eavp::Buffer buffer = eavp::Buffer::create(storage, valid).take_value();
    EXPECT_EQ(eavp::StatusCode::kUnsupported,
              buffer.map_plane(0U, eavp::MapMode::kReadOnly).status().code());
}

TEST(BufferTest, RejectsZeroStrideAndOverlappingPlanes) {
    std::shared_ptr<eavp::BufferStorage> storage(new UnmappableStorage(64U));
    std::vector<eavp::PlaneLayout> zero_stride;
    zero_stride.push_back(eavp::PlaneLayout(0U, 32U, 0U));
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::Buffer::create(storage, zero_stride).status().code());

    std::vector<eavp::PlaneLayout> overlapping;
    overlapping.push_back(eavp::PlaneLayout(0U, 32U, 16U));
    overlapping.push_back(eavp::PlaneLayout(16U, 32U, 16U));
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::Buffer::create(storage, overlapping).status().code());
}

TEST(BufferTest, UnmapsWhenMappedStorageCannotCoverRequestedPlane) {
    std::shared_ptr<ShortMappingStorage> storage(new ShortMappingStorage());
    std::vector<eavp::PlaneLayout> planes;
    planes.push_back(eavp::PlaneLayout(4U, 4U, 4U));
    eavp::Buffer buffer = eavp::Buffer::create(storage, planes).take_value();

    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              buffer.map_plane(0U, eavp::MapMode::kReadOnly).status().code());
    EXPECT_EQ(1, storage->unmap_count());
}

TEST(BufferTest, MovingMappedRegionUnmapsStorageOnlyOnce) {
    std::shared_ptr<CountingMappingStorage> storage(new CountingMappingStorage());
    std::vector<eavp::PlaneLayout> planes;
    planes.push_back(eavp::PlaneLayout(0U, 8U, 8U));
    eavp::Buffer buffer = eavp::Buffer::create(storage, planes).take_value();

    {
        eavp::MappedRegion mapped =
            buffer.map_plane(0U, eavp::MapMode::kReadOnly).take_value();
        eavp::MappedRegion moved(std::move(mapped));
        EXPECT_EQ(8U, moved.size());
    }
    EXPECT_EQ(1, storage->unmap_count());
}

TEST(BufferTest, ReadOnlyMappingDoesNotExposeMutableDataAndUnmapsOnce) {
    std::shared_ptr<CountingMappingStorage> storage(new CountingMappingStorage());
    const std::vector<eavp::PlaneLayout> planes{
        eavp::PlaneLayout(0U, 8U, 8U)};
    eavp::Buffer buffer = eavp::Buffer::create(storage, planes).take_value();

    {
        eavp::MappedRegion mapped =
            buffer.map_plane(0U, eavp::MapMode::kReadOnly).take_value();
        EXPECT_NE(static_cast<const std::uint8_t*>(NULL), mapped.data());
        EXPECT_EQ(static_cast<std::uint8_t*>(NULL), mapped.mutable_data());
        eavp::MappedRegion moved(std::move(mapped));
        EXPECT_EQ(static_cast<std::uint8_t*>(NULL), moved.mutable_data());
    }
    EXPECT_EQ(1, storage->unmap_count());
}

TEST(BufferTest, RejectsInvalidPlaneLayoutIndex) {
    eavp::Result<eavp::Buffer> allocated = eavp::Buffer::allocate(8U);
    ASSERT_TRUE(allocated.ok());
    eavp::Buffer buffer = allocated.take_value();

    const eavp::Result<eavp::PlaneLayout> layout = buffer.plane_layout(1U);

    EXPECT_EQ(eavp::StatusCode::kInvalidArgument, layout.status().code());
}

TEST(BufferTest, MappedRegionKeepsStorageAliveAfterBufferDestruction) {
    std::weak_ptr<CountingMappingStorage> weak_storage;
    std::unique_ptr<eavp::MappedRegion> mapped;
    {
        std::shared_ptr<CountingMappingStorage> storage(new CountingMappingStorage());
        weak_storage = storage;
        std::vector<eavp::PlaneLayout> planes;
        planes.push_back(eavp::PlaneLayout(0U, 8U, 8U));
        eavp::Result<eavp::Buffer> created = eavp::Buffer::create(storage, planes);
        ASSERT_TRUE(created.ok());
        eavp::Buffer buffer = created.take_value();
        eavp::Result<eavp::MappedRegion> mapped_result =
            buffer.map_plane(0U, eavp::MapMode::kReadOnly);
        ASSERT_TRUE(mapped_result.ok());
        mapped.reset(new eavp::MappedRegion(mapped_result.take_value()));
    }

    std::shared_ptr<CountingMappingStorage> retained_storage = weak_storage.lock();
    ASSERT_TRUE(retained_storage);
    EXPECT_EQ(0, retained_storage->unmap_count());
    mapped.reset();
    EXPECT_EQ(1, retained_storage->unmap_count());
}

TEST(BufferTest, ExportedDmaBufHandleClosesOnlyDuplicateAfterMove) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(0, pipe(fds));
    const int original_fd = fds[0];
    std::shared_ptr<eavp::BufferStorage> storage(new DmaBufStorage(original_fd));
    std::vector<eavp::PlaneLayout> planes;
    planes.push_back(eavp::PlaneLayout(0U, 1U, 1U));
    eavp::Buffer buffer = eavp::Buffer::create(storage, planes).take_value();

    int exported_fd = -1;
    {
        eavp::NativeBufferHandle exported = buffer.export_dmabuf().take_value();
        exported_fd = exported.file_descriptor();
        ASSERT_NE(original_fd, exported_fd);
        eavp::NativeBufferHandle moved(std::move(exported));
        EXPECT_EQ(exported_fd, moved.file_descriptor());
    }
    errno = 0;
    EXPECT_EQ(-1, fcntl(exported_fd, F_GETFD));
    const int fcntl_error = errno;
    EXPECT_EQ(EBADF, fcntl_error);
    EXPECT_NE(-1, fcntl(original_fd, F_GETFD));

    EXPECT_EQ(0, close(fds[0]));
    EXPECT_EQ(0, close(fds[1]));
}

TEST(BufferTest, SliceRequiresOnePlaneAndRevalidatesSelectedPlaneBounds) {
    std::shared_ptr<eavp::BufferStorage> storage(new UnmappableStorage(64U));
    std::vector<eavp::PlaneLayout> planes;
    planes.push_back(eavp::PlaneLayout(0U, 16U, 16U));
    planes.push_back(eavp::PlaneLayout(16U, 16U, 16U));
    eavp::Buffer buffer = eavp::Buffer::create(storage, planes).take_value();

    EXPECT_EQ(eavp::StatusCode::kUnsupported, buffer.slice(0U, 8U).status().code());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              buffer.slice_plane(1U, 12U, 8U).status().code());
}

TEST(BufferTest, SliceSharesStorageAndChecksBounds) {
    eavp::Buffer buffer = eavp::Buffer::allocate(8U).take_value();
    {
        eavp::MappedRegion mapped =
            buffer.map_plane(0U, eavp::MapMode::kReadWrite).take_value();
        mapped.mutable_data()[3] = 0x5a;
    }

    eavp::Buffer slice = buffer.slice(2U, 4U).take_value();
    eavp::Result<eavp::MappedRegion> mapped_result =
        slice.map_plane(0U, eavp::MapMode::kReadOnly);
    ASSERT_TRUE(mapped_result.ok());
    eavp::MappedRegion mapped = mapped_result.take_value();
    ASSERT_EQ(4U, mapped.size());
    EXPECT_EQ(0x5a, mapped.data()[1]);
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument, buffer.slice(7U, 2U).status().code());
}

TEST(MediaPacketTest, CopyRetainsSharedPayloadAndMetadata) {
    eavp::Buffer buffer = eavp::Buffer::allocate(4U).take_value();
    {
        eavp::MappedRegion mapped =
            buffer.map_plane(0U, eavp::MapMode::kReadWrite).take_value();
        mapped.mutable_data()[0] = 0x3c;
    }
    const eavp::TimeBase time_base = eavp::TimeBase::create(1, 90000).take_value();
    const eavp::MediaPacket packet = eavp::MediaPacket::create(
        buffer, eavp::CodecId::kReference, eavp::EncodedStreamFormat::kReference, 0, 9000,
        9000, 3600, time_base, true, eavp::CodecConfigData()).take_value();
    const eavp::MediaPacket copy = packet;

    eavp::MappedRegion mapped =
        copy.buffer().map_plane(0U, eavp::MapMode::kReadOnly).take_value();
    EXPECT_EQ(0x3c, mapped.data()[0]);
    EXPECT_EQ(eavp::CodecId::kReference, copy.codec());
    EXPECT_EQ(9000, copy.pts());
    EXPECT_EQ(3600, copy.duration());
    EXPECT_TRUE(copy.key_frame());
    EXPECT_EQ(eavp::EncodedStreamFormat::kReference, copy.stream_format());
    EXPECT_EQ(0, copy.stream_index());
}

TEST(VideoFormatTest, ValidatesPixelFormatPlaneCountStrideAndSize) {
    std::vector<eavp::PlaneLayout> nv12_planes;
    nv12_planes.push_back(eavp::PlaneLayout(0U, 128U, 16U));
    nv12_planes.push_back(eavp::PlaneLayout(128U, 64U, 16U));
    EXPECT_TRUE(eavp::VideoFormat::create(eavp::PixelFormat::kNv12, 16, 8,
                                           eavp::MemoryDomain::kCpu, nv12_planes)
                    .ok());

    nv12_planes.pop_back();
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::VideoFormat::create(eavp::PixelFormat::kNv12, 16, 8,
                                         eavp::MemoryDomain::kCpu, nv12_planes)
                  .status()
                  .code());

    std::vector<eavp::PlaneLayout> rgb24_planes;
    rgb24_planes.push_back(eavp::PlaneLayout(0U, 383U, 48U));
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::VideoFormat::create(eavp::PixelFormat::kRgb24, 16, 8,
                                         eavp::MemoryDomain::kCpu, rgb24_planes)
                  .status()
                  .code());

    rgb24_planes[0] = eavp::PlaneLayout(0U, 384U, 48U);
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::VideoFormat::create(eavp::PixelFormat::kRgb24, 16, 8,
                                         static_cast<eavp::MemoryDomain>(99), rgb24_planes)
                  .status()
                  .code());
}

TEST(VideoFormatTest, RejectsOddChromaDimensions) {
    std::vector<eavp::PlaneLayout> nv12_planes;
    nv12_planes.push_back(eavp::PlaneLayout(0U, 144U, 16U));
    nv12_planes.push_back(eavp::PlaneLayout(144U, 80U, 16U));

    EXPECT_EQ(eavp::StatusCode::kUnsupported,
              eavp::VideoFormat::create(eavp::PixelFormat::kNv12, 16, 9,
                                         eavp::MemoryDomain::kCpu, nv12_planes)
                  .status()
                  .code());
}

TEST(VideoFormatTest, StableStateNamesCoverSupportedEnumsAndRejectUnknownValues) {
    EXPECT_EQ("rgb24",
              eavp::pixel_format_name(eavp::PixelFormat::kRgb24)
                  .value());
    EXPECT_EQ("nv12",
              eavp::pixel_format_name(eavp::PixelFormat::kNv12).value());
    EXPECT_EQ("yuv420p",
              eavp::pixel_format_name(eavp::PixelFormat::kYuv420p).value());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::pixel_format_name(eavp::PixelFormat::kUnknown)
                  .status().code());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::pixel_format_name(static_cast<eavp::PixelFormat>(999))
                  .status().code());

    EXPECT_EQ("cpu",
              eavp::memory_domain_name(eavp::MemoryDomain::kCpu).value());
    EXPECT_EQ("mmap",
              eavp::memory_domain_name(eavp::MemoryDomain::kMmap).value());
    EXPECT_EQ("dmabuf",
              eavp::memory_domain_name(eavp::MemoryDomain::kDmaBuf).value());
    EXPECT_EQ("device_opaque",
              eavp::memory_domain_name(eavp::MemoryDomain::kDeviceOpaque)
                  .value());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::memory_domain_name(static_cast<eavp::MemoryDomain>(999))
                  .status().code());
}

TEST(VideoFormatTest, ValidatesYuv420pAndRejectsPlaneSizeOverflow) {
    std::vector<eavp::PlaneLayout> yuv420p_planes;
    yuv420p_planes.push_back(eavp::PlaneLayout(0U, 128U, 16U));
    yuv420p_planes.push_back(eavp::PlaneLayout(128U, 32U, 8U));
    yuv420p_planes.push_back(eavp::PlaneLayout(160U, 32U, 8U));
    EXPECT_TRUE(eavp::VideoFormat::create(eavp::PixelFormat::kYuv420p, 16, 8,
                                           eavp::MemoryDomain::kCpu, yuv420p_planes)
                    .ok());

    std::vector<eavp::PlaneLayout> overflowing_planes;
    overflowing_planes.push_back(eavp::PlaneLayout(
        0U, std::numeric_limits<std::size_t>::max(), std::numeric_limits<std::size_t>::max()));
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::VideoFormat::create(eavp::PixelFormat::kRgb24, 1, 2,
                                         eavp::MemoryDomain::kCpu, overflowing_planes)
                  .status()
                  .code());
}

TEST(VideoFormatTest, RejectsOverlappingAndOverflowingPlaneRanges) {
    const std::vector<eavp::PlaneLayout> overlapping_nv12{
        eavp::PlaneLayout(0U, 128U, 16U),
        eavp::PlaneLayout(64U, 64U, 16U)};
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::VideoFormat::create(eavp::PixelFormat::kNv12, 16, 8,
                                         eavp::MemoryDomain::kCpu,
                                         overlapping_nv12)
                  .status().code());

    const std::vector<eavp::PlaneLayout> overlapping_yuv420p{
        eavp::PlaneLayout(0U, 128U, 16U),
        eavp::PlaneLayout(128U, 32U, 8U),
        eavp::PlaneLayout(144U, 32U, 8U)};
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::VideoFormat::create(eavp::PixelFormat::kYuv420p, 16, 8,
                                         eavp::MemoryDomain::kCpu,
                                         overlapping_yuv420p)
                  .status().code());

    const std::vector<eavp::PlaneLayout> overflowing_nv12{
        eavp::PlaneLayout(std::numeric_limits<std::size_t>::max() - 63U,
                          128U, 16U),
        eavp::PlaneLayout(0U, 64U, 16U)};
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::VideoFormat::create(eavp::PixelFormat::kNv12, 16, 8,
                                         eavp::MemoryDomain::kCpu,
                                         overflowing_nv12)
                  .status().code());
}

TEST(FrameTest, VideoFrameRequiresBufferFormatDomainAndPlaneLayoutMatch) {
    std::shared_ptr<eavp::BufferStorage> storage(new UnmappableStorage(192U));
    std::vector<eavp::PlaneLayout> planes;
    planes.push_back(eavp::PlaneLayout(0U, 128U, 16U));
    planes.push_back(eavp::PlaneLayout(128U, 64U, 16U));
    const eavp::Buffer video_buffer = eavp::Buffer::create(storage, planes).take_value();
    const eavp::VideoFormat format = eavp::VideoFormat::create(
        eavp::PixelFormat::kNv12, 16, 8, eavp::MemoryDomain::kDeviceOpaque, planes)
                                          .take_value();
    const eavp::TimeBase video_time_base = eavp::TimeBase::create(1, 90000).take_value();
    const eavp::Result<eavp::VideoFrame> video =
        eavp::VideoFrame::create(video_buffer, format, 9000, video_time_base);
    ASSERT_TRUE(video.ok());
    EXPECT_EQ(16, video.value().format().width());
    EXPECT_EQ(8, video.value().format().height());

    std::vector<eavp::PlaneLayout> single_plane;
    single_plane.push_back(eavp::PlaneLayout(0U, 192U, 16U));
    const eavp::Buffer mismatched_buffer =
        eavp::Buffer::create(storage, single_plane).take_value();
    EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch,
              eavp::VideoFrame::create(mismatched_buffer, format, 0, video_time_base)
                  .status()
                  .code());

    std::vector<eavp::PlaneLayout> layout_mismatch;
    layout_mismatch.push_back(eavp::PlaneLayout(0U, 128U, 16U));
    layout_mismatch.push_back(eavp::PlaneLayout(129U, 63U, 16U));
    const eavp::Buffer layout_mismatched_buffer =
        eavp::Buffer::create(storage, layout_mismatch).take_value();
    EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch,
              eavp::VideoFrame::create(layout_mismatched_buffer, format, 0, video_time_base)
                  .status()
                  .code());

    std::shared_ptr<eavp::BufferStorage> mmap_storage(
        new UnmappableStorage(192U, eavp::MemoryDomain::kMmap));
    const eavp::Buffer domain_mismatched_buffer =
        eavp::Buffer::create(mmap_storage, planes).take_value();
    EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch,
              eavp::VideoFrame::create(domain_mismatched_buffer, format, 0, video_time_base)
                  .status()
                  .code());
}

TEST(VideoProcessorConfigTest, RejectsInvalidCropAndRotation) {
    std::vector<eavp::PlaneLayout> rgb24_planes;
    rgb24_planes.push_back(eavp::PlaneLayout(0U, 384U, 48U));
    const eavp::VideoFormat format = eavp::VideoFormat::create(
        eavp::PixelFormat::kRgb24, 16, 8, eavp::MemoryDomain::kCpu, rgb24_planes)
                                          .take_value();
    EXPECT_TRUE(eavp::VideoProcessorConfig::create(format, format, 0, 0, 16, 8, 0).ok());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::VideoProcessorConfig::create(format, format, -1, 0, 16, 8, 0)
                  .status()
                  .code());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::VideoProcessorConfig::create(format, format, 0, 0, 0, 8, 0)
                  .status()
                  .code());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::VideoProcessorConfig::create(format, format, 1, 0, 16, 8, 0)
                  .status()
                  .code());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::VideoProcessorConfig::create(format, format, 0, 0, 16, 8, 45)
                  .status()
                  .code());
}

TEST(MediaPacketTest, RejectsCodecAndStreamFormatMismatch) {
    eavp::Buffer payload = eavp::Buffer::allocate(4U).take_value();
    const eavp::TimeBase time_base = eavp::TimeBase::create(1, 90000).take_value();

    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::MediaPacket::create(payload, eavp::CodecId::kH264,
                                         eavp::EncodedStreamFormat::kHvcc, 0, 0, 0, 3600,
                                         time_base, true, eavp::CodecConfigData())
                  .status()
                  .code());
}

TEST(VideoEncoderConfigTest, ValidatesTimeBaseAndFrameRate) {
    const eavp::TimeBase time_base = eavp::TimeBase::create(1, 90000).take_value();
    EXPECT_TRUE(eavp::VideoEncoderConfig::create(
                    eavp::CodecId::kH264, 16, 8, 30, 1, time_base, 1000000, 1200000, 30, 0,
                    eavp::RateControlMode::kCbr, eavp::CodecProfile::kH264Main, 40, true)
                    .ok());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::VideoEncoderConfig::create(
                  eavp::CodecId::kH264, 16, 8, 0, 1, time_base, 1000000, 1200000, 30, 0,
                  eavp::RateControlMode::kCbr, eavp::CodecProfile::kH264Main, 40, true)
                  .status()
                  .code());
}

TEST(VideoEncoderConfigTest, RejectsInvalidAndCrossCodecProfiles) {
    const eavp::TimeBase time_base = eavp::TimeBase::create(1, 90000).take_value();
    const eavp::CodecProfile invalid_profile =
        static_cast<eavp::CodecProfile>(999);

    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::VideoEncoderConfig::create(
                  eavp::CodecId::kH264, 16, 16, 30, 1, time_base, 1000, 1000,
                  30, 0, eavp::RateControlMode::kCbr, invalid_profile, 40,
                  false).status().code());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::VideoEncoderConfig::create(
                  eavp::CodecId::kH264, 16, 16, 30, 1, time_base, 1000, 1000,
                  30, 0, eavp::RateControlMode::kCbr,
                  eavp::CodecProfile::kH265Main, 40, false).status().code());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::VideoEncoderConfig::create(
                  eavp::CodecId::kH265, 16, 16, 30, 1, time_base, 1000, 1000,
                  30, 0, eavp::RateControlMode::kCbr,
                  eavp::CodecProfile::kH264High, 40, false).status().code());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::VideoEncoderConfig::create(
                  eavp::CodecId::kReference, 16, 16, 30, 1, time_base, 1, 1,
                  1, 0, eavp::RateControlMode::kConstantQuality,
                  eavp::CodecProfile::kH264Baseline, 0, true).status().code());
}

TEST(AudioFormatTest, DescribesSupportedInterleavedFormatsExactly) {
    struct Case {
        eavp::SampleFormat sample_format;
        std::size_t bytes_per_sample;
    };
    const Case cases[] = {
        {eavp::SampleFormat::kSigned16LittleEndian, 2U},
        {eavp::SampleFormat::kSigned24In32LittleEndian, 4U},
        {eavp::SampleFormat::kSigned32LittleEndian, 4U},
        {eavp::SampleFormat::kFloat32LittleEndian, 4U},
    };
    for (std::size_t index = 0U; index < 4U; ++index) {
        eavp::Result<eavp::AudioFormat> format = eavp::AudioFormat::create(
            cases[index].sample_format, 48000,
            eavp::AudioChannelLayout::kStereo,
            eavp::AudioSampleLayout::kInterleaved,
            eavp::MemoryDomain::kCpu);
        ASSERT_TRUE(format.ok());
        EXPECT_EQ(2, format.value().channels());
        EXPECT_EQ(cases[index].bytes_per_sample,
                  format.value().bytes_per_sample());
        EXPECT_EQ(cases[index].bytes_per_sample * 2U,
                  format.value().bytes_per_pcm_frame());
    }
}

TEST(AudioFormatTest, RejectsInvalidConfigurations) {
    struct Case {
        eavp::SampleFormat sample_format;
        int sample_rate;
        eavp::AudioChannelLayout channel_layout;
        eavp::AudioSampleLayout sample_layout;
        eavp::MemoryDomain memory_domain;
    };
    const Case cases[] = {
        {eavp::SampleFormat::kUnknown, 48000,
         eavp::AudioChannelLayout::kStereo,
         eavp::AudioSampleLayout::kInterleaved, eavp::MemoryDomain::kCpu},
        {eavp::SampleFormat::kSigned16LittleEndian, 0,
         eavp::AudioChannelLayout::kStereo,
         eavp::AudioSampleLayout::kInterleaved, eavp::MemoryDomain::kCpu},
        {static_cast<eavp::SampleFormat>(99), 48000,
         eavp::AudioChannelLayout::kStereo,
         eavp::AudioSampleLayout::kInterleaved, eavp::MemoryDomain::kCpu},
        {eavp::SampleFormat::kSigned16LittleEndian, 48000,
         static_cast<eavp::AudioChannelLayout>(99),
         eavp::AudioSampleLayout::kInterleaved, eavp::MemoryDomain::kCpu},
        {eavp::SampleFormat::kSigned16LittleEndian, 48000,
         eavp::AudioChannelLayout::kStereo,
         static_cast<eavp::AudioSampleLayout>(99), eavp::MemoryDomain::kCpu},
        {eavp::SampleFormat::kSigned16LittleEndian, 48000,
         eavp::AudioChannelLayout::kStereo,
         eavp::AudioSampleLayout::kInterleaved,
         static_cast<eavp::MemoryDomain>(99)},
    };
    for (std::size_t index = 0U; index < 6U; ++index) {
        EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
                  eavp::AudioFormat::create(
                      cases[index].sample_format, cases[index].sample_rate,
                      cases[index].channel_layout, cases[index].sample_layout,
                      cases[index].memory_domain).status().code());
    }
}

TEST(AudioFrameTest, UsesFirstSamplePtsAndExactPayloadSize) {
    const eavp::AudioFormat format = eavp::AudioFormat::create(
        eavp::SampleFormat::kSigned16LittleEndian, 48000,
        eavp::AudioChannelLayout::kStereo,
        eavp::AudioSampleLayout::kInterleaved,
        eavp::MemoryDomain::kCpu).take_value();
    const eavp::Buffer buffer = eavp::Buffer::allocate(1920U).take_value();
    const eavp::AudioFrame frame = eavp::AudioFrame::create(
        buffer, format, 480, 1234000,
        eavp::TimeBase::create(1, 1000000).take_value(), true).take_value();

    EXPECT_EQ(480, frame.samples_per_channel());
    EXPECT_EQ(1234000, frame.pts());
    EXPECT_TRUE(frame.discontinuity());
    EXPECT_EQ(48000, frame.format().sample_rate());
}

TEST(AudioFrameTest, RejectsWrongBufferAndNonPositiveTimeBase) {
    const eavp::AudioFormat format = eavp::AudioFormat::create(
        eavp::SampleFormat::kSigned16LittleEndian, 48000,
        eavp::AudioChannelLayout::kStereo,
        eavp::AudioSampleLayout::kInterleaved,
        eavp::MemoryDomain::kCpu).take_value();
    EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch,
              eavp::AudioFrame::create(
                  eavp::Buffer::allocate(1919U).take_value(), format, 480, 0,
                  eavp::TimeBase::create(1, 1000000).take_value(), false)
                  .status().code());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::AudioFrame::create(
                  eavp::Buffer::allocate(1920U).take_value(), format, 480, 0,
                  eavp::TimeBase::create(0, 1000000).take_value(), false)
                  .status().code());
}

TEST(AudioFrameTest, RejectsInvalidShapeAndBufferCompatibility) {
    const eavp::AudioFormat cpu_format = eavp::AudioFormat::create(
        eavp::SampleFormat::kSigned16LittleEndian, 48000,
        eavp::AudioChannelLayout::kStereo,
        eavp::AudioSampleLayout::kInterleaved,
        eavp::MemoryDomain::kCpu).take_value();
    const eavp::AudioFormat mmap_format = eavp::AudioFormat::create(
        eavp::SampleFormat::kSigned16LittleEndian, 48000,
        eavp::AudioChannelLayout::kStereo,
        eavp::AudioSampleLayout::kInterleaved,
        eavp::MemoryDomain::kMmap).take_value();
    std::shared_ptr<eavp::BufferStorage> storage(new UnmappableStorage(1920U));
    const std::vector<eavp::PlaneLayout> two_planes{
        eavp::PlaneLayout(0U, 960U, 960U),
        eavp::PlaneLayout(960U, 960U, 960U)};
    const eavp::Buffer buffer_with_two_planes =
        eavp::Buffer::create(storage, two_planes).take_value();

    struct Case {
        eavp::Buffer buffer;
        eavp::AudioFormat format;
        int samples_per_channel;
        eavp::StatusCode expected_status;
    };
    const Case cases[] = {
        {eavp::Buffer::allocate(1920U).take_value(), cpu_format, 0,
         eavp::StatusCode::kInvalidArgument},
        {buffer_with_two_planes, cpu_format, 480,
         eavp::StatusCode::kCapabilityMismatch},
        {eavp::Buffer::allocate(1920U).take_value(), mmap_format, 480,
         eavp::StatusCode::kCapabilityMismatch},
    };
    const eavp::TimeBase time_base = eavp::TimeBase::create(1, 1000000).take_value();
    for (std::size_t index = 0U; index < 3U; ++index) {
        EXPECT_EQ(cases[index].expected_status,
                  eavp::AudioFrame::create(
                      cases[index].buffer, cases[index].format,
                      cases[index].samples_per_channel, 0, time_base, false)
                      .status().code());
    }
}

TEST(AudioFrameTest, RejectsOverflowingPayloadSizeWhenRepresentable) {
    const eavp::AudioFormat format = eavp::AudioFormat::create(
        eavp::SampleFormat::kFloat32LittleEndian, 48000,
        eavp::AudioChannelLayout::kStereo,
        eavp::AudioSampleLayout::kInterleaved,
        eavp::MemoryDomain::kCpu).take_value();
    const eavp::TimeBase time_base = eavp::TimeBase::create(1, 1000000).take_value();

    if (sizeof(std::size_t) == 4U) {
        const eavp::Result<eavp::AudioFrame> frame = eavp::AudioFrame::create(
            eavp::Buffer::allocate(1U).take_value(), format,
            std::numeric_limits<int>::max(), 0, time_base, false);
        EXPECT_EQ(eavp::StatusCode::kInvalidArgument, frame.status().code());
        EXPECT_EQ("audio frame size overflows", frame.status().message());
    } else {
        EXPECT_LE(static_cast<std::size_t>(std::numeric_limits<int>::max()),
                  std::numeric_limits<std::size_t>::max() / 8U);
    }
}

}  // namespace
