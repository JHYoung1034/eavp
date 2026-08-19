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
    const eavp::MediaPacket packet(buffer, eavp::CodecId::kH264, 9000, 9000, 3600,
                                   time_base, true);
    const eavp::MediaPacket copy = packet;

    eavp::MappedRegion mapped =
        copy.buffer().map_plane(0U, eavp::MapMode::kReadOnly).take_value();
    EXPECT_EQ(0x3c, mapped.data()[0]);
    EXPECT_EQ(eavp::CodecId::kH264, copy.codec());
    EXPECT_EQ(9000, copy.pts());
    EXPECT_EQ(3600, copy.duration());
    EXPECT_TRUE(copy.key_frame());
}

TEST(FrameTest, VideoAndAudioFramesValidateShapeAndShareBuffer) {
    eavp::Buffer video_buffer = eavp::Buffer::allocate(128U).take_value();
    {
        eavp::MappedRegion mapped =
            video_buffer.map_plane(0U, eavp::MapMode::kReadWrite).take_value();
        mapped.mutable_data()[0] = 0x7e;
    }
    const eavp::TimeBase video_time_base = eavp::TimeBase::create(1, 90000).take_value();
    const eavp::Result<eavp::VideoFrame> video = eavp::VideoFrame::create(
        video_buffer, eavp::PixelFormat::kNv12, 16, 8, 16, 9000, video_time_base);
    ASSERT_TRUE(video.ok());
    eavp::MappedRegion video_mapped = video.value().buffer()
                                           .map_plane(0U, eavp::MapMode::kReadOnly)
                                           .take_value();
    EXPECT_EQ(0x7e, video_mapped.data()[0]);
    EXPECT_EQ(16, video.value().width());
    EXPECT_EQ(8, video.value().height());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::VideoFrame::create(video_buffer, eavp::PixelFormat::kNv12, 0, 8, 16,
                                       0, video_time_base).status().code());

    const eavp::Buffer audio_buffer = eavp::Buffer::allocate(64U).take_value();
    const eavp::Result<eavp::AudioFrame> audio = eavp::AudioFrame::create(
        audio_buffer, eavp::SampleFormat::kSigned16, 48000, 2, 16, 0,
        eavp::TimeBase::create(1, 48000).take_value());
    ASSERT_TRUE(audio.ok());
    EXPECT_EQ(48000, audio.value().sample_rate());
    EXPECT_EQ(2, audio.value().channels());
}

}  // namespace
