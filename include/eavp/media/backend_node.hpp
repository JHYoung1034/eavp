#ifndef EAVP_MEDIA_BACKEND_NODE_HPP_
#define EAVP_MEDIA_BACKEND_NODE_HPP_

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "eavp/media/backend.hpp"
#include "eavp/media/node.hpp"
#include "eavp/media/port.hpp"

namespace eavp {

class VideoProcessorNode : public MediaNode {
public:
    VideoProcessorNode(const std::string& id,
                       std::unique_ptr<VideoProcessor> processor,
                       const VideoProcessorConfig& config,
                       std::size_t input_capacity);

    InputPort<VideoFrame>& input();
    OutputPort<VideoFrame>& output();

protected:
    Status on_prepare() override;
    Status on_stop() override;
    Status on_reset() override;
    Status on_tick() override;

private:
    Status send_pending_output();

    std::unique_ptr<VideoProcessor> processor_;
    VideoProcessorConfig config_;
    InputPort<VideoFrame> input_;
    OutputPort<VideoFrame> output_;
    std::shared_ptr<const VideoFrame> pending_input_;
    std::shared_ptr<const VideoFrame> pending_output_;
    bool drain_started_;
};

class VideoEncoderNode : public MediaNode {
public:
    VideoEncoderNode(const std::string& id,
                     std::unique_ptr<VideoEncoder> encoder,
                     const VideoFormat& input_format,
                     const VideoEncoderConfig& config,
                     std::size_t input_capacity);

    InputPort<VideoFrame>& input();
    OutputPort<MediaPacket>& output();

protected:
    Status on_prepare() override;
    Status on_stop() override;
    Status on_reset() override;
    Status on_tick() override;

private:
    Status send_pending_output();

    std::unique_ptr<VideoEncoder> encoder_;
    VideoFormat input_format_;
    VideoEncoderConfig config_;
    InputPort<VideoFrame> input_;
    OutputPort<MediaPacket> output_;
    std::shared_ptr<const VideoFrame> pending_input_;
    std::shared_ptr<const MediaPacket> pending_output_;
    bool drain_started_;
};

}  // namespace eavp

#endif  // EAVP_MEDIA_BACKEND_NODE_HPP_
