#ifndef EAVP_PLATFORM_LINUX_V4L2_CAPTURE_HPP_
#define EAVP_PLATFORM_LINUX_V4L2_CAPTURE_HPP_

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "eavp/base/result.hpp"
#include "eavp/media/frame.hpp"
#include "eavp/media/node.hpp"
#include "eavp/media/port.hpp"
#include "eavp/media/video_format.hpp"
#include "eavp/platform/linux/wait_source.hpp"

namespace eavp {

class MetricRegistry;

class V4L2CaptureConfig {
public:
    static Result<V4L2CaptureConfig> create(
        const std::string& device_path, PixelFormat pixel_format,
        int width, int height, int frame_rate_numerator,
        int frame_rate_denominator, std::size_t buffer_count);

    const std::string& device_path() const { return device_path_; }
    PixelFormat pixel_format() const { return pixel_format_; }
    int width() const { return width_; }
    int height() const { return height_; }
    int frame_rate_numerator() const { return frame_rate_numerator_; }
    int frame_rate_denominator() const { return frame_rate_denominator_; }
    std::size_t buffer_count() const { return buffer_count_; }

private:
    V4L2CaptureConfig(const std::string& device_path, PixelFormat pixel_format,
                      int width, int height, int frame_rate_numerator,
                      int frame_rate_denominator, std::size_t buffer_count)
        : device_path_(device_path), pixel_format_(pixel_format), width_(width),
          height_(height), frame_rate_numerator_(frame_rate_numerator),
          frame_rate_denominator_(frame_rate_denominator), buffer_count_(buffer_count) {}

    std::string device_path_;
    PixelFormat pixel_format_;
    int width_;
    int height_;
    int frame_rate_numerator_;
    int frame_rate_denominator_;
    std::size_t buffer_count_;
};

class V4L2SourceNode : public MediaNode, public LinuxWaitSource {
public:
    static Result<std::unique_ptr<V4L2SourceNode> > create(
        const std::string& id, const V4L2CaptureConfig& config,
        MetricRegistry* metrics);
    ~V4L2SourceNode() noexcept;

    OutputPort<VideoFrame>& output();
    Result<VideoFormat> actual_format() const;
    Result<std::vector<struct pollfd> > poll_descriptors() override;
    Result<bool> evaluate_poll_events(
        const std::vector<struct pollfd>& descriptors) override;

protected:
    Status on_prepare() override;
    Status on_start() override;
    Status on_stop() override;
    Status on_reset() override;
    Status on_tick() override;

private:
    class Impl;

    V4L2SourceNode(const std::string& id, std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace eavp

#endif  // EAVP_PLATFORM_LINUX_V4L2_CAPTURE_HPP_
