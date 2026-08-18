#include <gtest/gtest.h>

#include <cstdint>

#include "eavp/media/buffer.hpp"
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

}  // namespace
