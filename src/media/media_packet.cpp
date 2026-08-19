#include "eavp/media/media_packet.hpp"

#include <new>

namespace eavp {

namespace {

bool is_valid_stream_format(CodecId codec, EncodedStreamFormat stream_format) {
    switch (codec) {
    case CodecId::kH264:
        return stream_format == EncodedStreamFormat::kAnnexB ||
               stream_format == EncodedStreamFormat::kAvcc;
    case CodecId::kH265:
        return stream_format == EncodedStreamFormat::kAnnexB ||
               stream_format == EncodedStreamFormat::kHvcc;
    case CodecId::kReference:
        return stream_format == EncodedStreamFormat::kReference;
    case CodecId::kUnknown:
    case CodecId::kAac:
        return false;
    }
    return false;
}

}  // namespace

Result<MediaPacket> MediaPacket::create(const Buffer& buffer, CodecId codec,
                                        EncodedStreamFormat stream_format, int stream_index,
                                        std::int64_t pts, std::int64_t dts,
                                        std::int64_t duration, const TimeBase& time_base,
                                        bool key_frame, const CodecConfigData& codec_config) {
    if (stream_index < 0 || duration < 0) {
        return Result<MediaPacket>(
            Status(StatusCode::kInvalidArgument, "packet stream index and duration are invalid"));
    }
    if (!is_valid_stream_format(codec, stream_format)) {
        return Result<MediaPacket>(
            Status(StatusCode::kInvalidArgument, "packet codec and stream format are incompatible"));
    }
    try {
        return Result<MediaPacket>(MediaPacket(buffer, codec, stream_format, stream_index, pts,
                                                dts, duration, time_base, key_frame,
                                                codec_config));
    } catch (const std::bad_alloc&) {
        return Result<MediaPacket>(
            Status(StatusCode::kResourceExhausted, "failed to create packet metadata"));
    }
}

}  // namespace eavp
