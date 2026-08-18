#ifndef EAVP_MEDIA_MEDIA_PACKET_HPP_
#define EAVP_MEDIA_MEDIA_PACKET_HPP_

#include <cstdint>

#include "eavp/base/time.hpp"
#include "eavp/media/buffer.hpp"

namespace eavp {

enum class CodecId {
    kUnknown,
    kH264,
    kH265,
    kAac,
};

class MediaPacket {
public:
    MediaPacket(const Buffer& buffer, CodecId codec, std::int64_t pts, std::int64_t dts,
                std::int64_t duration, const TimeBase& time_base, bool key_frame)
        : buffer_(buffer),
          codec_(codec),
          pts_(pts),
          dts_(dts),
          duration_(duration),
          time_base_(time_base),
          key_frame_(key_frame) {}

    const Buffer& buffer() const { return buffer_; }
    CodecId codec() const { return codec_; }
    std::int64_t pts() const { return pts_; }
    std::int64_t dts() const { return dts_; }
    std::int64_t duration() const { return duration_; }
    const TimeBase& time_base() const { return time_base_; }
    bool key_frame() const { return key_frame_; }

private:
    Buffer buffer_;
    CodecId codec_;
    std::int64_t pts_;
    std::int64_t dts_;
    std::int64_t duration_;
    TimeBase time_base_;
    bool key_frame_;
};

}  // namespace eavp

#endif  // EAVP_MEDIA_MEDIA_PACKET_HPP_

