#ifndef EAVP_TESTS_SUPPORT_FAKE_V4L2_API_HPP_
#define EAVP_TESTS_SUPPORT_FAKE_V4L2_API_HPP_

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <linux/videodev2.h>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <stdexcept>
#include <sys/mman.h>
#include <vector>

#include "platform/linux/v4l2_api.hpp"

namespace eavp_test {

struct FakeV4L2Buffer {
    FakeV4L2Buffer(std::uint32_t index_value, std::uint32_t length_value,
                   std::uint32_t offset_value, std::uint32_t type_value =
                       V4L2_BUF_TYPE_VIDEO_CAPTURE,
                   std::uint32_t memory_value = V4L2_MEMORY_MMAP)
        : index(index_value), length(length_value), offset(offset_value),
          type(type_value), memory(memory_value) {}

    std::uint32_t index;
    std::uint32_t length;
    std::uint32_t offset;
    std::uint32_t type;
    std::uint32_t memory;
};

struct FakeV4L2MapCall {
    FakeV4L2MapCall(void* address_value, std::size_t length_value,
                    int protection_value, int flags_value, int fd_value,
                    std::int64_t offset_value)
        : address(address_value), length(length_value),
          protection(protection_value), flags(flags_value), fd(fd_value),
          offset(offset_value) {}

    void* address;
    std::size_t length;
    int protection;
    int flags;
    int fd;
    std::int64_t offset;
};

struct FakeV4L2Trace {
    FakeV4L2Trace()
        : open_calls(0U), close_calls(0U), request_buffers_zero_calls(0U),
          stream_on_calls(0U), stream_off_calls(0U), last_open_flags(0),
          requested_format_type(0U),
          requested_pixel_format(0U), requested_width(0U), requested_height(0U),
          requested_field(0U), requested_frame_numerator(0U),
          requested_frame_denominator(0U), requested_parameter_type(0U),
          requested_buffer_count(0U), requested_buffer_type(0U),
          requested_memory_type(0U) {}

    std::vector<std::string> operations;
    std::vector<unsigned long> ioctl_requests;
    std::vector<FakeV4L2MapCall> mmap_calls;
    std::vector<FakeV4L2MapCall> munmap_calls;
    std::size_t open_calls;
    std::size_t close_calls;
    std::size_t request_buffers_zero_calls;
    std::size_t stream_on_calls;
    std::size_t stream_off_calls;
    int last_open_flags;
    std::string last_open_path;
    std::uint32_t requested_format_type;
    std::uint32_t requested_pixel_format;
    std::uint32_t requested_width;
    std::uint32_t requested_height;
    std::uint32_t requested_field;
    std::uint32_t requested_frame_numerator;
    std::uint32_t requested_frame_denominator;
    std::uint32_t requested_parameter_type;
    std::uint32_t requested_buffer_count;
    std::uint32_t requested_buffer_type;
    std::uint32_t requested_memory_type;
    std::vector<std::uint32_t> queried_indices;
    std::vector<std::uint32_t> queried_buffer_types;
    std::vector<std::uint32_t> queried_memory_types;
    std::vector<std::uint32_t> stream_on_types;
    std::vector<std::uint32_t> stream_off_types;
};

class FakeV4L2Api : public eavp::detail::V4L2Api {
public:
    explicit FakeV4L2Api(const std::shared_ptr<FakeV4L2Trace>& trace)
        : trace_(trace), last_error_(0), descriptor_(41),
          capabilities_(V4L2_CAP_DEVICE_CAPS),
          device_capabilities_(V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING),
          pixel_format_(V4L2_PIX_FMT_YUV420), width_(16U), height_(8U),
          bytes_per_line_(32U), size_image_(384U), frame_numerator_(1U),
          frame_denominator_(30U), returned_buffer_count_(3U),
          returned_buffer_type_(V4L2_BUF_TYPE_VIDEO_CAPTURE),
          returned_memory_type_(V4L2_MEMORY_MMAP),
          maximum_mappable_offset_(
              std::numeric_limits<std::uint64_t>::max()),
          monotonic_now_result_(0) {
        buffers_.push_back(FakeV4L2Buffer(0U, 384U, 0U));
        buffers_.push_back(FakeV4L2Buffer(1U, 384U, 4096U));
        buffers_.push_back(FakeV4L2Buffer(2U, 384U, 8192U));
        monotonic_now_value_.tv_sec = 1;
        monotonic_now_value_.tv_nsec = 0;
    }

    void set_capabilities(std::uint32_t capabilities,
                          std::uint32_t device_capabilities) {
        capabilities_ = capabilities;
        device_capabilities_ = device_capabilities;
    }

    void set_format(std::uint32_t pixel_format, std::uint32_t width,
                    std::uint32_t height, std::uint32_t bytes_per_line,
                    std::uint32_t size_image) {
        pixel_format_ = pixel_format;
        width_ = width;
        height_ = height;
        bytes_per_line_ = bytes_per_line;
        size_image_ = size_image;
        for (std::size_t index = 0U; index < buffers_.size(); ++index) {
            buffers_[index].length = size_image;
        }
    }

    void set_frame_interval(std::uint32_t numerator,
                            std::uint32_t denominator) {
        frame_numerator_ = numerator;
        frame_denominator_ = denominator;
    }

    void set_returned_buffer_count(std::uint32_t count) {
        returned_buffer_count_ = count;
        while (buffers_.size() < static_cast<std::size_t>(count)) {
            const std::uint32_t index =
                static_cast<std::uint32_t>(buffers_.size());
            buffers_.push_back(FakeV4L2Buffer(
                index, size_image_, static_cast<std::uint32_t>(index * 4096U)));
        }
    }

    void set_returned_buffer_metadata(std::uint32_t type,
                                      std::uint32_t memory) {
        returned_buffer_type_ = type;
        returned_memory_type_ = memory;
    }

    void set_buffer(std::size_t requested_index, std::uint32_t reported_index,
                    std::uint32_t length, std::uint32_t offset,
                    std::uint32_t type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                    std::uint32_t memory = V4L2_MEMORY_MMAP) {
        while (buffers_.size() <= requested_index) {
            const std::uint32_t index =
                static_cast<std::uint32_t>(buffers_.size());
            buffers_.push_back(FakeV4L2Buffer(index, size_image_, index * 4096U));
        }
        buffers_[requested_index] =
            FakeV4L2Buffer(reported_index, length, offset, type, memory);
    }

    void set_maximum_mappable_offset(std::uint64_t value) {
        maximum_mappable_offset_ = value;
    }

    std::uint64_t maximum_mappable_offset() const override {
        return maximum_mappable_offset_;
    }

    void script_error(const std::string& operation, int native_error) {
        errors_[operation].push_back(native_error);
    }

    void repeat_error(const std::string& operation, int native_error,
                      std::size_t count) {
        for (std::size_t index = 0U; index < count; ++index) {
            script_error(operation, native_error);
        }
    }

    void script_throw(const std::string& operation) {
        ++throws_[operation];
    }

    int open_device(const char* path, int flags) override {
        ++trace_->open_calls;
        trace_->last_open_path = path == NULL ? std::string() : std::string(path);
        trace_->last_open_flags = flags;
        trace_->operations.push_back("open");
        throw_if_scripted("open");
        if (take_error("open")) return -1;
        return descriptor_;
    }

    int device_ioctl(int fd, unsigned long request, void* argument) override {
        (void)fd;
        trace_->ioctl_requests.push_back(request);
        const std::string operation = ioctl_operation(request, argument);
        trace_->operations.push_back(operation);
        if (operation == "VIDIOC_REQBUFS(0)") {
            ++trace_->request_buffers_zero_calls;
        } else if (request == VIDIOC_REQBUFS) {
            const struct v4l2_requestbuffers* request_buffers =
                static_cast<const struct v4l2_requestbuffers*>(argument);
            trace_->requested_buffer_count = request_buffers->count;
            trace_->requested_buffer_type = request_buffers->type;
            trace_->requested_memory_type = request_buffers->memory;
        } else if (request == VIDIOC_QUERYBUF) {
            const struct v4l2_buffer* buffer =
                static_cast<const struct v4l2_buffer*>(argument);
            trace_->queried_indices.push_back(buffer->index);
            trace_->queried_buffer_types.push_back(buffer->type);
            trace_->queried_memory_types.push_back(buffer->memory);
        } else if (request == VIDIOC_STREAMON) {
            ++trace_->stream_on_calls;
            trace_->stream_on_types.push_back(
                *static_cast<const enum v4l2_buf_type*>(argument));
        } else if (request == VIDIOC_STREAMOFF) {
            ++trace_->stream_off_calls;
            trace_->stream_off_types.push_back(
                *static_cast<const enum v4l2_buf_type*>(argument));
        }
        throw_if_scripted(operation);
        if (take_error(operation)) return -1;

        if (request == VIDIOC_QUERYCAP) {
            struct v4l2_capability* capability =
                static_cast<struct v4l2_capability*>(argument);
            std::memset(capability, 0, sizeof(*capability));
            capability->capabilities = capabilities_;
            capability->device_caps = device_capabilities_;
        } else if (request == VIDIOC_S_FMT) {
            const struct v4l2_format* format =
                static_cast<const struct v4l2_format*>(argument);
            trace_->requested_format_type = format->type;
            trace_->requested_pixel_format = format->fmt.pix.pixelformat;
            trace_->requested_width = format->fmt.pix.width;
            trace_->requested_height = format->fmt.pix.height;
            trace_->requested_field = format->fmt.pix.field;
        } else if (request == VIDIOC_G_FMT) {
            struct v4l2_format* format =
                static_cast<struct v4l2_format*>(argument);
            format->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            format->fmt.pix.width = width_;
            format->fmt.pix.height = height_;
            format->fmt.pix.pixelformat = pixel_format_;
            format->fmt.pix.bytesperline = bytes_per_line_;
            format->fmt.pix.sizeimage = size_image_;
        } else if (request == VIDIOC_S_PARM) {
            const struct v4l2_streamparm* parameters =
                static_cast<const struct v4l2_streamparm*>(argument);
            trace_->requested_parameter_type = parameters->type;
            trace_->requested_frame_numerator =
                parameters->parm.capture.timeperframe.numerator;
            trace_->requested_frame_denominator =
                parameters->parm.capture.timeperframe.denominator;
        } else if (request == VIDIOC_G_PARM) {
            struct v4l2_streamparm* parameters =
                static_cast<struct v4l2_streamparm*>(argument);
            parameters->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            parameters->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
            parameters->parm.capture.timeperframe.numerator = frame_numerator_;
            parameters->parm.capture.timeperframe.denominator = frame_denominator_;
        } else if (request == VIDIOC_REQBUFS) {
            struct v4l2_requestbuffers* request_buffers =
                static_cast<struct v4l2_requestbuffers*>(argument);
            if (request_buffers->count != 0U) {
                request_buffers->count = returned_buffer_count_;
                request_buffers->type = returned_buffer_type_;
                request_buffers->memory = returned_memory_type_;
            }
        } else if (request == VIDIOC_QUERYBUF) {
            struct v4l2_buffer* buffer =
                static_cast<struct v4l2_buffer*>(argument);
            const std::size_t requested_index =
                static_cast<std::size_t>(buffer->index);
            if (requested_index >= buffers_.size()) {
                last_error_ = EINVAL;
                return -1;
            }
            const FakeV4L2Buffer value = buffers_[requested_index];
            buffer->type = value.type;
            buffer->memory = value.memory;
            buffer->index = value.index;
            buffer->length = value.length;
            buffer->m.offset = value.offset;
        }
        return 0;
    }

    void* map_memory(void* address, std::size_t length, int protection,
                     int flags, int fd, std::int64_t offset) override {
        trace_->operations.push_back("mmap");
        trace_->mmap_calls.push_back(
            FakeV4L2MapCall(address, length, protection, flags, fd, offset));
        throw_if_scripted("mmap");
        if (take_error("mmap")) return MAP_FAILED;
        const std::uintptr_t mapped_address =
            static_cast<std::uintptr_t>(0x10000U) +
            trace_->mmap_calls.size() * static_cast<std::uintptr_t>(0x10000U);
        return reinterpret_cast<void*>(mapped_address);
    }

    int unmap_memory(void* address, std::size_t length) override {
        trace_->operations.push_back("munmap");
        trace_->munmap_calls.push_back(
            FakeV4L2MapCall(address, length, 0, 0, -1, 0));
        throw_if_scripted("munmap");
        return take_error("munmap") ? -1 : 0;
    }

    int close_device(int fd) override {
        (void)fd;
        ++trace_->close_calls;
        trace_->operations.push_back("close");
        throw_if_scripted("close");
        return take_error("close") ? -1 : 0;
    }

    int monotonic_now(struct timespec* value) override {
        trace_->operations.push_back("clock_gettime(CLOCK_MONOTONIC)");
        throw_if_scripted("clock_gettime(CLOCK_MONOTONIC)");
        if (take_error("clock_gettime(CLOCK_MONOTONIC)")) return -1;
        *value = monotonic_now_value_;
        return monotonic_now_result_;
    }

    int last_error() const override { return last_error_; }

private:
    void throw_if_scripted(const std::string& operation) {
        std::map<std::string, std::size_t>::iterator found =
            throws_.find(operation);
        if (found == throws_.end() || found->second == 0U) return;
        --found->second;
        throw std::runtime_error("scripted V4L2 API exception");
    }

    bool take_error(const std::string& operation) {
        std::map<std::string, std::deque<int> >::iterator found =
            errors_.find(operation);
        if (found == errors_.end() || found->second.empty()) return false;
        const int error = found->second.front();
        found->second.pop_front();
        if (error == 0) return false;
        last_error_ = error;
        return true;
    }

    static std::string ioctl_operation(unsigned long request, void* argument) {
        if (request == VIDIOC_QUERYCAP) return "VIDIOC_QUERYCAP";
        if (request == VIDIOC_S_FMT) return "VIDIOC_S_FMT";
        if (request == VIDIOC_G_FMT) return "VIDIOC_G_FMT";
        if (request == VIDIOC_S_PARM) return "VIDIOC_S_PARM";
        if (request == VIDIOC_G_PARM) return "VIDIOC_G_PARM";
        if (request == VIDIOC_REQBUFS) {
            const struct v4l2_requestbuffers* request_buffers =
                static_cast<const struct v4l2_requestbuffers*>(argument);
            return request_buffers->count == 0U ? "VIDIOC_REQBUFS(0)"
                                                : "VIDIOC_REQBUFS";
        }
        if (request == VIDIOC_QUERYBUF) return "VIDIOC_QUERYBUF";
        if (request == VIDIOC_STREAMON) return "VIDIOC_STREAMON";
        if (request == VIDIOC_STREAMOFF) return "VIDIOC_STREAMOFF";
        return "ioctl";
    }

    std::shared_ptr<FakeV4L2Trace> trace_;
    int last_error_;
    int descriptor_;
    std::uint32_t capabilities_;
    std::uint32_t device_capabilities_;
    std::uint32_t pixel_format_;
    std::uint32_t width_;
    std::uint32_t height_;
    std::uint32_t bytes_per_line_;
    std::uint32_t size_image_;
    std::uint32_t frame_numerator_;
    std::uint32_t frame_denominator_;
    std::uint32_t returned_buffer_count_;
    std::uint32_t returned_buffer_type_;
    std::uint32_t returned_memory_type_;
    std::uint64_t maximum_mappable_offset_;
    std::vector<FakeV4L2Buffer> buffers_;
    std::map<std::string, std::deque<int> > errors_;
    std::map<std::string, std::size_t> throws_;
    int monotonic_now_result_;
    struct timespec monotonic_now_value_;
};

}  // namespace eavp_test

#endif  // EAVP_TESTS_SUPPORT_FAKE_V4L2_API_HPP_
