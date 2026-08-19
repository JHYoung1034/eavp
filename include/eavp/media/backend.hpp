#ifndef EAVP_MEDIA_BACKEND_HPP_
#define EAVP_MEDIA_BACKEND_HPP_

#include <memory>
#include <thread>

#include "eavp/media/capability.hpp"
#include "eavp/media/frame.hpp"
#include "eavp/media/media_packet.hpp"

namespace eavp {

enum class BackendState {
    kCreated,
    kConfigured,
    kRunning,
    kDraining,
    kStopped,
    kError,
};

class BackendThreadAffinity {
public:
    BackendThreadAffinity()
        : thread_bound_(false), owner_thread_() {}

protected:
    ~BackendThreadAffinity() {}

    Status bind_to_current_thread() {
        if (!thread_bound_) {
            owner_thread_ = std::this_thread::get_id();
            thread_bound_ = true;
            return Status::ok_status();
        }
        return verify_current_thread();
    }

    Status verify_current_thread() const {
        if (thread_bound_ && owner_thread_ != std::this_thread::get_id()) {
            return Status(
                StatusCode::kInvalidState,
                "backend instance called from a different executor thread");
        }
        return Status::ok_status();
    }

private:
    BackendThreadAffinity(const BackendThreadAffinity&) = delete;
    BackendThreadAffinity& operator=(const BackendThreadAffinity&) = delete;

    bool thread_bound_;
    std::thread::id owner_thread_;
};

class VideoProcessor : protected BackendThreadAffinity {
public:
    virtual ~VideoProcessor() {}

    virtual BackendState state() const = 0;
    virtual Status configure(const VideoProcessorConfig& config) = 0;
    virtual Status submit(
        const std::shared_ptr<const VideoFrame>& frame) = 0;
    virtual Result<std::shared_ptr<const VideoFrame> > receive() = 0;
    virtual Status begin_drain() = 0;
    virtual Status reset() = 0;

protected:
    VideoProcessor() {}

private:
    VideoProcessor(const VideoProcessor&) = delete;
    VideoProcessor& operator=(const VideoProcessor&) = delete;
};

class VideoEncoder : protected BackendThreadAffinity {
public:
    virtual ~VideoEncoder() {}

    virtual BackendState state() const = 0;
    virtual Status configure(const VideoFormat& input,
                             const VideoEncoderConfig& config) = 0;
    virtual Status submit(
        const std::shared_ptr<const VideoFrame>& frame) = 0;
    virtual Result<std::shared_ptr<const MediaPacket> > receive() = 0;
    virtual Status begin_drain() = 0;
    virtual Status reset() = 0;

protected:
    VideoEncoder() {}

private:
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;
};

class MediaBackendProvider {
public:
    virtual ~MediaBackendProvider() {}

    virtual Result<ProviderCapability> probe() const = 0;
    virtual Result<std::unique_ptr<VideoProcessor> > create_video_processor()
        const = 0;
    virtual Result<std::unique_ptr<VideoEncoder> > create_video_encoder()
        const = 0;

protected:
    MediaBackendProvider() {}

private:
    MediaBackendProvider(const MediaBackendProvider&) = delete;
    MediaBackendProvider& operator=(const MediaBackendProvider&) = delete;
};

}  // namespace eavp

#endif  // EAVP_MEDIA_BACKEND_HPP_
