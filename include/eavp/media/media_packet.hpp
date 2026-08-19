#ifndef EAVP_MEDIA_MEDIA_PACKET_HPP_
#define EAVP_MEDIA_MEDIA_PACKET_HPP_

#include <cstdint>

#include "eavp/base/time.hpp"
#include "eavp/media/buffer.hpp"
#include "eavp/media/video_codec.hpp"

namespace eavp {

class MediaPacket {
public:
    static Result<MediaPacket> create(const Buffer& buffer, CodecId codec,
                                      EncodedStreamFormat stream_format, int stream_index,
                                      std::int64_t pts, std::int64_t dts, std::int64_t duration,
                                      const TimeBase& time_base, bool key_frame,
                                      const CodecConfigData& codec_config);

    const Buffer& buffer() const { return buffer_; }
    CodecId codec() const { return codec_; }
    EncodedStreamFormat stream_format() const { return stream_format_; }
    int stream_index() const { return stream_index_; }
    std::int64_t pts() const { return pts_; }
    std::int64_t dts() const { return dts_; }
    std::int64_t duration() const { return duration_; }
    const TimeBase& time_base() const { return time_base_; }
    bool key_frame() const { return key_frame_; }
    const CodecConfigData& codec_config() const { return codec_config_; }

private:
    MediaPacket(const Buffer& buffer, CodecId codec, EncodedStreamFormat stream_format,
                int stream_index, std::int64_t pts, std::int64_t dts, std::int64_t duration,
                const TimeBase& time_base, bool key_frame, const CodecConfigData& codec_config)
        : buffer_(buffer),
          codec_(codec),
          stream_format_(stream_format),
          stream_index_(stream_index),
          pts_(pts),
          dts_(dts),
          duration_(duration),
          time_base_(time_base),
          key_frame_(key_frame),
          codec_config_(codec_config) {}

    Buffer buffer_;
    CodecId codec_;
    EncodedStreamFormat stream_format_;
    int stream_index_;
    std::int64_t pts_;
    std::int64_t dts_;
    std::int64_t duration_;
    TimeBase time_base_;
    bool key_frame_;
    CodecConfigData codec_config_;
};

}  // namespace eavp

#endif  // EAVP_MEDIA_MEDIA_PACKET_HPP_
