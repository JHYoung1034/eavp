#include <gtest/gtest.h>

#include <cstdint>

#include "eavp/media/buffer.hpp"
#include "eavp/media/frame.hpp"
#include "eavp/media/media_packet.hpp"

namespace {

TEST(BufferTest, SliceSharesStorageAndChecksBounds) {
    eavp::Result<eavp::Buffer> allocated = eavp::Buffer::allocate(8);
    ASSERT_TRUE(allocated.ok());
    eavp::Buffer buffer = allocated.value();
    buffer.mutable_data()[3] = 0x5a;

    eavp::Result<eavp::Buffer> slice = buffer.slice(2, 4);
    ASSERT_TRUE(slice.ok());
    EXPECT_EQ(4U, slice.value().size());
    EXPECT_EQ(buffer.data() + 2, slice.value().data());
    EXPECT_EQ(0x5a, slice.value().data()[1]);

    EXPECT_EQ(eavp::StatusCode::kInvalidArgument, buffer.slice(7, 2).status().code());
}

TEST(MediaPacketTest, CopyRetainsSharedPayloadAndMetadata) {
    eavp::Buffer buffer = eavp::Buffer::allocate(4).value();
    const eavp::TimeBase time_base = eavp::TimeBase::create(1, 90000).value();
    const eavp::MediaPacket packet(buffer, eavp::CodecId::kH264, 9000, 9000, 3600,
                                   time_base, true);
    const eavp::MediaPacket copy = packet;

    EXPECT_EQ(packet.buffer().data(), copy.buffer().data());
    EXPECT_EQ(eavp::CodecId::kH264, copy.codec());
    EXPECT_EQ(9000, copy.pts());
    EXPECT_EQ(3600, copy.duration());
    EXPECT_TRUE(copy.key_frame());
}

TEST(FrameTest, VideoAndAudioFramesValidateShapeAndShareBuffer) {
    const eavp::Buffer video_buffer = eavp::Buffer::allocate(128U).value();
    const eavp::TimeBase video_time_base = eavp::TimeBase::create(1, 90000).value();
    const eavp::Result<eavp::VideoFrame> video = eavp::VideoFrame::create(
        video_buffer, eavp::PixelFormat::kNv12, 16, 8, 16, 9000, video_time_base);
    ASSERT_TRUE(video.ok());
    EXPECT_EQ(video_buffer.data(), video.value().buffer().data());
    EXPECT_EQ(16, video.value().width());
    EXPECT_EQ(8, video.value().height());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::VideoFrame::create(video_buffer, eavp::PixelFormat::kNv12, 0, 8, 16,
                                       0, video_time_base).status().code());

    const eavp::Buffer audio_buffer = eavp::Buffer::allocate(64U).value();
    const eavp::Result<eavp::AudioFrame> audio = eavp::AudioFrame::create(
        audio_buffer, eavp::SampleFormat::kSigned16, 48000, 2, 16, 0,
        eavp::TimeBase::create(1, 48000).value());
    ASSERT_TRUE(audio.ok());
    EXPECT_EQ(48000, audio.value().sample_rate());
    EXPECT_EQ(2, audio.value().channels());
}

}  // namespace
