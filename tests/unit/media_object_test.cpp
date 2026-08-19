#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
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
    explicit UnmappableStorage(std::size_t capacity) : capacity_(capacity), provider_id_("test") {}

    eavp::MemoryDomain memory_domain() const override {
        return eavp::MemoryDomain::kDeviceOpaque;
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
        buffer, eavp::CodecId::kH264, eavp::EncodedStreamFormat::kAnnexB, 0, 9000, 9000,
        3600, time_base, true, eavp::CodecConfigData()).take_value();
    const eavp::MediaPacket copy = packet;

    eavp::MappedRegion mapped =
        copy.buffer().map_plane(0U, eavp::MapMode::kReadOnly).take_value();
    EXPECT_EQ(0x3c, mapped.data()[0]);
    EXPECT_EQ(eavp::CodecId::kH264, copy.codec());
    EXPECT_EQ(9000, copy.pts());
    EXPECT_EQ(3600, copy.duration());
    EXPECT_TRUE(copy.key_frame());
    EXPECT_EQ(eavp::EncodedStreamFormat::kAnnexB, copy.stream_format());
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

TEST(FrameTest, AudioFramesValidateShapeAndShareBuffer) {
    const eavp::Buffer audio_buffer = eavp::Buffer::allocate(64U).take_value();
    const eavp::Result<eavp::AudioFrame> audio = eavp::AudioFrame::create(
        audio_buffer, eavp::SampleFormat::kSigned16, 48000, 2, 16, 0,
        eavp::TimeBase::create(1, 48000).take_value());
    ASSERT_TRUE(audio.ok());
    EXPECT_EQ(48000, audio.value().sample_rate());
    EXPECT_EQ(2, audio.value().channels());
}

}  // namespace
