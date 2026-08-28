#include "platform/linux/v4l2_system.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <linux/videodev2.h>
#include <new>
#include <sys/mman.h>
#include <sys/types.h>
#include <utility>

namespace eavp {
namespace detail {
namespace {

const int kMaximumInterruptedAttempts = 64;

StatusCode system_error_code(int native_code) {
    if (native_code == ENOENT) return StatusCode::kNotFound;
    if (native_code == ENODEV || native_code == ENXIO) {
        return StatusCode::kDeviceLost;
    }
    if (native_code == ENOMEM) return StatusCode::kResourceExhausted;
    return StatusCode::kIoError;
}

StatusCode dequeue_error_code(int native_code) {
    if (native_code == EAGAIN) return StatusCode::kWouldBlock;
    if (native_code == ENODEV || native_code == ENXIO || native_code == EIO) {
        return StatusCode::kDeviceLost;
    }
    return system_error_code(native_code);
}

Status v4l2_failure(StatusCode code, const char* message,
                    const char* operation, int native_code) {
    return Status(code, message, "v4l2", operation, native_code);
}

Status syscall_failure(const char* operation, int native_code) {
    return v4l2_failure(system_error_code(native_code),
                        std::strerror(native_code), operation, native_code);
}

Status contract_failure(const char* operation, const char* message) {
    return v4l2_failure(StatusCode::kCapabilityMismatch, message,
                        operation, 0);
}

Status dequeue_failure(int native_code) {
    return v4l2_failure(dequeue_error_code(native_code),
                        std::strerror(native_code),
                        "VIDIOC_DQBUF", native_code);
}

void remember_first(Status* first, const Status& candidate) {
    if (first->ok() && !candidate.ok()) *first = candidate;
}

bool checked_add(std::size_t left, std::size_t right,
                 std::size_t* result) {
    if (left > std::numeric_limits<std::size_t>::max() - right) return false;
    *result = left + right;
    return true;
}

bool checked_multiply(std::size_t left, std::size_t right,
                      std::size_t* result) {
    if (left != 0U &&
        right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    *result = left * right;
    return true;
}

bool checked_multiply_u64(std::uint64_t left, std::uint64_t right,
                          std::uint64_t* result) {
    if (left != 0U &&
        right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    *result = left * right;
    return true;
}

bool equivalent_frame_interval(std::uint32_t actual_numerator,
                               std::uint32_t actual_denominator,
                               int requested_fps_numerator,
                               int requested_fps_denominator) {
    if (actual_numerator == 0U || actual_denominator == 0U ||
        requested_fps_numerator <= 0 || requested_fps_denominator <= 0) {
        return false;
    }
    std::uint64_t actual_scaled = 0U;
    std::uint64_t requested_scaled = 0U;
    return checked_multiply_u64(
               static_cast<std::uint64_t>(actual_numerator),
               static_cast<std::uint64_t>(requested_fps_numerator),
               &actual_scaled) &&
           checked_multiply_u64(
               static_cast<std::uint64_t>(actual_denominator),
               static_cast<std::uint64_t>(requested_fps_denominator),
               &requested_scaled) &&
           actual_scaled == requested_scaled;
}

bool uint32_to_size(std::uint32_t value, std::size_t* result) {
    if (static_cast<std::uint64_t>(value) >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    *result = static_cast<std::size_t>(value);
    return true;
}

bool size_to_uint32(std::size_t value, std::uint32_t* result) {
    if (value > static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max())) {
        return false;
    }
    *result = static_cast<std::uint32_t>(value);
    return true;
}

bool uint32_to_offset(std::uint32_t value,
                      std::uint64_t maximum_mappable_offset,
                      std::int64_t* result) {
    if (static_cast<std::uint64_t>(value) >
            maximum_mappable_offset ||
        static_cast<std::uint64_t>(value) >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    *result = static_cast<std::int64_t>(value);
    return true;
}

std::uint32_t v4l2_pixel_format(PixelFormat format, bool* supported) {
    *supported = true;
    switch (format) {
        case PixelFormat::kYuv420p:
            return V4L2_PIX_FMT_YUV420;
        case PixelFormat::kNv12:
            return V4L2_PIX_FMT_NV12;
        case PixelFormat::kYuyv422:
            return V4L2_PIX_FMT_YUYV;
        case PixelFormat::kUnknown:
        case PixelFormat::kRgb24:
            *supported = false;
            return 0U;
    }
    *supported = false;
    return 0U;
}

Status append_plane(std::size_t offset, std::size_t size,
                    std::size_t stride, std::size_t visible_row_bytes,
                    std::vector<PlaneLayout>* planes,
                    std::vector<std::size_t>* visible_rows,
                    std::vector<std::size_t>* source_offsets) {
    try {
        planes->push_back(PlaneLayout(offset, size, stride));
        visible_rows->push_back(visible_row_bytes);
        source_offsets->push_back(offset);
        return Status::ok_status();
    } catch (const std::bad_alloc&) {
        return v4l2_failure(StatusCode::kResourceExhausted,
                            "failed to allocate V4L2 format metadata",
                            "VIDIOC_G_FMT", ENOMEM);
    } catch (...) {
        return v4l2_failure(StatusCode::kInternal,
                            "failed to create V4L2 format metadata",
                            "VIDIOC_G_FMT", 0);
    }
}

Status make_negotiated_format(
    PixelFormat pixel_format, int width, int height,
    const struct v4l2_pix_format& pixel,
    std::unique_ptr<V4L2NegotiatedFormat>* negotiated) {
    std::size_t stride = 0U;
    std::size_t capacity = 0U;
    if (!uint32_to_size(pixel.bytesperline, &stride) ||
        !uint32_to_size(pixel.sizeimage, &capacity)) {
        return contract_failure("VIDIOC_G_FMT",
                                "V4L2 format sizes are not representable");
    }
    const std::size_t visible_width = static_cast<std::size_t>(width);
    const std::size_t rows = static_cast<std::size_t>(height);
    std::vector<PlaneLayout> planes;
    std::vector<std::size_t> visible_rows;
    std::vector<std::size_t> source_offsets;
    try {
        const std::size_t plane_count =
            pixel_format == PixelFormat::kYuv420p
                ? 3U
                : (pixel_format == PixelFormat::kNv12 ? 2U : 1U);
        planes.reserve(plane_count);
        visible_rows.reserve(plane_count);
        source_offsets.reserve(plane_count);
    } catch (const std::bad_alloc&) {
        return v4l2_failure(StatusCode::kResourceExhausted,
                            "failed to allocate V4L2 format metadata",
                            "VIDIOC_G_FMT", ENOMEM);
    }

    std::size_t first_size = 0U;
    std::size_t second_size = 0U;
    std::size_t second_offset = 0U;
    std::size_t third_offset = 0U;
    std::size_t required_end = 0U;
    Status append_status;
    switch (pixel_format) {
        case PixelFormat::kYuv420p: {
            if (stride < visible_width || (stride % 2U) != 0U ||
                !checked_multiply(stride, rows, &first_size)) {
                return contract_failure(
                    "VIDIOC_G_FMT", "V4L2 YUV420 padding is invalid");
            }
            const std::size_t chroma_stride = stride / 2U;
            if (!checked_multiply(chroma_stride, rows / 2U, &second_size) ||
                !checked_add(first_size, second_size, &third_offset) ||
                !checked_add(third_offset, second_size, &required_end)) {
                return contract_failure(
                    "VIDIOC_G_FMT", "V4L2 YUV420 layout overflows");
            }
            second_offset = first_size;
            append_status = append_plane(
                0U, first_size, stride, visible_width, &planes,
                &visible_rows, &source_offsets);
            if (!append_status.ok()) return append_status;
            append_status = append_plane(
                second_offset, second_size, chroma_stride,
                visible_width / 2U, &planes, &visible_rows, &source_offsets);
            if (!append_status.ok()) return append_status;
            append_status = append_plane(
                third_offset, second_size, chroma_stride,
                visible_width / 2U, &planes, &visible_rows, &source_offsets);
            if (!append_status.ok()) return append_status;
            break;
        }
        case PixelFormat::kNv12:
            if (stride < visible_width ||
                !checked_multiply(stride, rows, &first_size) ||
                !checked_multiply(stride, rows / 2U, &second_size) ||
                !checked_add(first_size, second_size, &required_end)) {
                return contract_failure(
                    "VIDIOC_G_FMT", "V4L2 NV12 padding is invalid or overflows");
            }
            second_offset = first_size;
            append_status = append_plane(
                0U, first_size, stride, visible_width, &planes,
                &visible_rows, &source_offsets);
            if (!append_status.ok()) return append_status;
            append_status = append_plane(
                second_offset, second_size, stride, visible_width,
                &planes, &visible_rows, &source_offsets);
            if (!append_status.ok()) return append_status;
            break;
        case PixelFormat::kYuyv422: {
            std::size_t visible_bytes = 0U;
            if (!checked_multiply(visible_width, 2U, &visible_bytes) ||
                stride < visible_bytes ||
                !checked_multiply(stride, rows, &required_end)) {
                return contract_failure(
                    "VIDIOC_G_FMT", "V4L2 YUYV padding is invalid or overflows");
            }
            append_status = append_plane(
                0U, required_end, stride, visible_bytes, &planes,
                &visible_rows, &source_offsets);
            if (!append_status.ok()) return append_status;
            break;
        }
        case PixelFormat::kUnknown:
        case PixelFormat::kRgb24:
            return contract_failure("VIDIOC_G_FMT",
                                    "V4L2 pixel format is unsupported");
    }

    if (capacity < required_end) {
        return contract_failure(
            "VIDIOC_G_FMT", "V4L2 sizeimage does not cover the final plane");
    }

    Result<VideoFormat> format = VideoFormat::create(
        pixel_format, width, height, MemoryDomain::kMmap, planes);
    if (!format.ok()) {
        return contract_failure(
            "VIDIOC_G_FMT", "V4L2 negotiated layout is not a valid video format");
    }
    try {
        negotiated->reset(new V4L2NegotiatedFormat(
            format.take_value(), capacity, std::move(visible_rows),
            std::move(source_offsets)));
        return Status::ok_status();
    } catch (const std::bad_alloc&) {
        return v4l2_failure(StatusCode::kResourceExhausted,
                            "failed to allocate V4L2 negotiated format",
                            "VIDIOC_G_FMT", ENOMEM);
    } catch (...) {
        return v4l2_failure(StatusCode::kInternal,
                            "failed to create V4L2 negotiated format",
                            "VIDIOC_G_FMT", 0);
    }
}

}  // namespace

V4L2System::V4L2System(std::unique_ptr<V4L2Api> api)
    : api_(std::move(api)), fd_(-1), regions_(),
      kernel_buffers_allocated_(false), negotiated_(), state_(kCreated) {}

V4L2System::~V4L2System() noexcept {
    cleanup_noexcept();
}

V4L2System::V4L2System(V4L2System&& other) noexcept
    : api_(std::move(other.api_)), fd_(other.fd_),
      regions_(std::move(other.regions_)),
      kernel_buffers_allocated_(other.kernel_buffers_allocated_),
      negotiated_(std::move(other.negotiated_)), state_(other.state_) {
    other.fd_ = -1;
    other.regions_.clear();
    other.kernel_buffers_allocated_ = false;
    other.state_ = kCreated;
}

V4L2System& V4L2System::operator=(V4L2System&& other) noexcept {
    if (this != &other) {
        cleanup_noexcept();
        api_ = std::move(other.api_);
        fd_ = other.fd_;
        regions_ = std::move(other.regions_);
        kernel_buffers_allocated_ = other.kernel_buffers_allocated_;
        negotiated_ = std::move(other.negotiated_);
        state_ = other.state_;
        other.fd_ = -1;
        other.regions_.clear();
        other.kernel_buffers_allocated_ = false;
        other.state_ = kCreated;
    }
    return *this;
}

int V4L2System::bounded_open(const char* path, int flags) {
    int result = -1;
    for (int attempt = 0; attempt < kMaximumInterruptedAttempts; ++attempt) {
        result = api_->open_device(path, flags);
        if (result >= 0 || api_->last_error() != EINTR) break;
    }
    return result;
}

int V4L2System::bounded_ioctl(unsigned long request, void* argument) {
    int result = -1;
    for (int attempt = 0; attempt < kMaximumInterruptedAttempts; ++attempt) {
        result = api_->device_ioctl(fd_, request, argument);
        if (result >= 0 || api_->last_error() != EINTR) break;
    }
    return result;
}

Status V4L2System::rollback(const Status& first_failure) {
    try {
        reset();
    } catch (...) {
        cleanup_noexcept();
    }
    return first_failure;
}

Status V4L2System::prepare(const V4L2CaptureConfig& config) {
    if (!api_) {
        return v4l2_failure(StatusCode::kInvalidState,
                            "V4L2 API is not configured", "prepare", 0);
    }
    if (state_ != kCreated || fd_ >= 0 || kernel_buffers_allocated_ ||
        !regions_.empty()) {
        return v4l2_failure(StatusCode::kInvalidState,
                            "V4L2 device is already prepared", "prepare", 0);
    }

    try {
        bool supported = false;
        const std::uint32_t pixel_format =
            v4l2_pixel_format(config.pixel_format(), &supported);
        if (!supported) {
            return contract_failure("VIDIOC_S_FMT",
                                    "V4L2 pixel format is unsupported");
        }

        fd_ = bounded_open(config.device_path().c_str(),
                           O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) {
            return rollback(syscall_failure("open", api_->last_error()));
        }

        struct v4l2_capability capability;
        std::memset(&capability, 0, sizeof(capability));
        if (bounded_ioctl(VIDIOC_QUERYCAP, &capability) < 0) {
            return rollback(syscall_failure(
                "VIDIOC_QUERYCAP", api_->last_error()));
        }
        const std::uint32_t active_capabilities =
            (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U
                ? capability.device_caps
                : capability.capabilities;
        const std::uint32_t required_capabilities =
            V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
        if ((active_capabilities & required_capabilities) !=
            required_capabilities) {
            return rollback(contract_failure(
                "VIDIOC_QUERYCAP",
                "V4L2 device lacks single-planar capture or streaming"));
        }

        struct v4l2_format requested_format;
        std::memset(&requested_format, 0, sizeof(requested_format));
        requested_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        requested_format.fmt.pix.width =
            static_cast<std::uint32_t>(config.width());
        requested_format.fmt.pix.height =
            static_cast<std::uint32_t>(config.height());
        requested_format.fmt.pix.pixelformat = pixel_format;
        requested_format.fmt.pix.field = V4L2_FIELD_ANY;
        if (bounded_ioctl(VIDIOC_S_FMT, &requested_format) < 0) {
            return rollback(syscall_failure(
                "VIDIOC_S_FMT", api_->last_error()));
        }

        struct v4l2_format actual_format;
        std::memset(&actual_format, 0, sizeof(actual_format));
        actual_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (bounded_ioctl(VIDIOC_G_FMT, &actual_format) < 0) {
            return rollback(syscall_failure(
                "VIDIOC_G_FMT", api_->last_error()));
        }
        if (actual_format.type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
            actual_format.fmt.pix.pixelformat != pixel_format ||
            actual_format.fmt.pix.width !=
                static_cast<std::uint32_t>(config.width()) ||
            actual_format.fmt.pix.height !=
                static_cast<std::uint32_t>(config.height())) {
            return rollback(contract_failure(
                "VIDIOC_G_FMT",
                "V4L2 driver changed the requested format or dimensions"));
        }

        std::unique_ptr<V4L2NegotiatedFormat> negotiated;
        const Status layout_status = make_negotiated_format(
            config.pixel_format(), config.width(), config.height(),
            actual_format.fmt.pix, &negotiated);
        if (!layout_status.ok()) return rollback(layout_status);

        struct v4l2_streamparm requested_parameters;
        std::memset(&requested_parameters, 0, sizeof(requested_parameters));
        requested_parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        requested_parameters.parm.capture.timeperframe.numerator =
            static_cast<std::uint32_t>(config.frame_rate_denominator());
        requested_parameters.parm.capture.timeperframe.denominator =
            static_cast<std::uint32_t>(config.frame_rate_numerator());
        if (bounded_ioctl(VIDIOC_S_PARM, &requested_parameters) < 0) {
            return rollback(syscall_failure(
                "VIDIOC_S_PARM", api_->last_error()));
        }

        struct v4l2_streamparm actual_parameters;
        std::memset(&actual_parameters, 0, sizeof(actual_parameters));
        actual_parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (bounded_ioctl(VIDIOC_G_PARM, &actual_parameters) < 0) {
            return rollback(syscall_failure(
                "VIDIOC_G_PARM", api_->last_error()));
        }
        if (actual_parameters.type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
            (actual_parameters.parm.capture.capability &
             V4L2_CAP_TIMEPERFRAME) == 0U ||
            !equivalent_frame_interval(
                actual_parameters.parm.capture.timeperframe.numerator,
                actual_parameters.parm.capture.timeperframe.denominator,
                config.frame_rate_numerator(),
                config.frame_rate_denominator())) {
            return rollback(contract_failure(
                "VIDIOC_G_PARM",
                "V4L2 driver changed the requested frame interval"));
        }

        struct v4l2_requestbuffers request_buffers;
        std::memset(&request_buffers, 0, sizeof(request_buffers));
        request_buffers.count =
            static_cast<std::uint32_t>(config.buffer_count());
        request_buffers.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        request_buffers.memory = V4L2_MEMORY_MMAP;
        if (bounded_ioctl(VIDIOC_REQBUFS, &request_buffers) < 0) {
            return rollback(syscall_failure(
                "VIDIOC_REQBUFS", api_->last_error()));
        }
        kernel_buffers_allocated_ = true;
        if (request_buffers.type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
            request_buffers.memory != V4L2_MEMORY_MMAP ||
            request_buffers.count < 2U) {
            return rollback(contract_failure(
                "VIDIOC_REQBUFS",
                "V4L2 driver returned an invalid MMAP buffer allocation"));
        }

        std::size_t buffer_count = 0U;
        if (!uint32_to_size(request_buffers.count, &buffer_count)) {
            return rollback(contract_failure(
                "VIDIOC_REQBUFS",
                "V4L2 buffer count is not representable"));
        }
        try {
            regions_.reserve(buffer_count);
        } catch (const std::bad_alloc&) {
            return rollback(v4l2_failure(
                StatusCode::kResourceExhausted,
                "failed to reserve V4L2 mapped buffer metadata",
                "VIDIOC_REQBUFS", ENOMEM));
        }

        for (std::size_t index = 0U; index < buffer_count; ++index) {
            std::uint32_t query_index = 0U;
            if (!size_to_uint32(index, &query_index)) {
                return rollback(contract_failure(
                    "VIDIOC_QUERYBUF", "V4L2 buffer index overflows"));
            }
            struct v4l2_buffer buffer;
            std::memset(&buffer, 0, sizeof(buffer));
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = query_index;
            if (bounded_ioctl(VIDIOC_QUERYBUF, &buffer) < 0) {
                return rollback(syscall_failure(
                    "VIDIOC_QUERYBUF", api_->last_error()));
            }

            std::size_t length = 0U;
            std::int64_t offset = 0;
            if (buffer.type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
                buffer.memory != V4L2_MEMORY_MMAP ||
                buffer.index != query_index ||
                !uint32_to_size(buffer.length, &length) ||
                length < negotiated->total_capacity ||
                !uint32_to_offset(
                    buffer.m.offset, api_->maximum_mappable_offset(),
                    &offset)) {
                return rollback(contract_failure(
                    "VIDIOC_QUERYBUF",
                    "V4L2 query buffer metadata is invalid or overflows"));
            }

            void* const address = api_->map_memory(
                NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, offset);
            if (address == MAP_FAILED) {
                return rollback(syscall_failure("mmap", api_->last_error()));
            }
            regions_.push_back(MappedRegion(address, length));
        }

        negotiated_ = std::move(negotiated);
        state_ = kPrepared;
        return Status::ok_status();
    } catch (const std::bad_alloc&) {
        return rollback(v4l2_failure(
            StatusCode::kResourceExhausted,
            "failed to allocate V4L2 session metadata", "prepare", ENOMEM));
    } catch (...) {
        return rollback(v4l2_failure(
            StatusCode::kInternal,
            "unexpected failure while preparing V4L2", "prepare", 0));
    }
}

Status V4L2System::start() {
    if (state_ == kRunning) return Status::ok_status();
    if (state_ != kPrepared || fd_ < 0 || !kernel_buffers_allocated_) {
        return v4l2_failure(StatusCode::kInvalidState,
                            "V4L2 device is not prepared",
                            "VIDIOC_STREAMON", 0);
    }
    std::size_t queued_count = 0U;
    for (std::size_t index = 0U; index < regions_.size(); ++index) {
        const Status queue_status = queue_buffer(
            static_cast<std::uint32_t>(index));
        if (!queue_status.ok()) {
            return rollback_start(queue_status, queued_count);
        }
        ++queued_count;
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (bounded_ioctl(VIDIOC_STREAMON, &type) < 0) {
        return rollback_start(
            syscall_failure("VIDIOC_STREAMON", api_->last_error()),
            queued_count);
    }
    state_ = kRunning;
    return Status::ok_status();
}

Status V4L2System::rollback_start(const Status& primary_failure,
                                  std::size_t queued_count) {
    if (queued_count == 0U) return primary_failure;

    bool rollback_failed = false;
    try {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        rollback_failed = bounded_ioctl(VIDIOC_STREAMOFF, &type) < 0;
    } catch (...) {
        rollback_failed = true;
    }
    if (rollback_failed) {
        // STREAMOFF 已无法恢复可重试的 Prepared 会话，完整释放资源。
        (void)reset();
    }
    return primary_failure;
}

Status V4L2System::queue_buffer(std::uint32_t index) {
    struct v4l2_buffer buffer;
    std::memset(&buffer, 0, sizeof(buffer));
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    if (bounded_ioctl(VIDIOC_QBUF, &buffer) < 0) {
        return syscall_failure("VIDIOC_QBUF", api_->last_error());
    }
    return Status::ok_status();
}

Result<V4L2DequeuedBuffer> V4L2System::dequeue() {
    if (state_ != kRunning || fd_ < 0 || !api_) {
        return Result<V4L2DequeuedBuffer>(v4l2_failure(
            StatusCode::kInvalidState,
            "V4L2 dequeue requires a running device",
            "VIDIOC_DQBUF", 0));
    }

    struct v4l2_buffer buffer;
    std::memset(&buffer, 0, sizeof(buffer));
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    if (bounded_ioctl(VIDIOC_DQBUF, &buffer) < 0) {
        return Result<V4L2DequeuedBuffer>(
            dequeue_failure(api_->last_error()));
    }
    if (static_cast<std::size_t>(buffer.index) >= regions_.size()) {
        return Result<V4L2DequeuedBuffer>(v4l2_failure(
            StatusCode::kCorruptData,
            "V4L2 dequeued an unknown buffer index",
            "VIDIOC_DQBUF", 0));
    }

    const MappedRegion& region = regions_[buffer.index];
    return Result<V4L2DequeuedBuffer>(V4L2DequeuedBuffer(
        buffer.index, static_cast<const std::uint8_t*>(region.address),
        region.length, buffer.bytesused, buffer.flags, buffer.sequence,
        buffer.timestamp));
}

Status V4L2System::requeue(std::uint32_t index) {
    if (state_ != kRunning || fd_ < 0 || !api_) {
        return v4l2_failure(StatusCode::kInvalidState,
                            "V4L2 requeue requires a running device",
                            "VIDIOC_QBUF", 0);
    }
    if (static_cast<std::size_t>(index) >= regions_.size()) {
        return v4l2_failure(StatusCode::kInvalidArgument,
                            "V4L2 requeue index is out of range",
                            "VIDIOC_QBUF", 0);
    }
    return queue_buffer(index);
}

Status V4L2System::stop() {
    if (state_ == kCreated || state_ == kPrepared) return Status::ok_status();
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (bounded_ioctl(VIDIOC_STREAMOFF, &type) < 0) {
        return syscall_failure("VIDIOC_STREAMOFF", api_->last_error());
    }
    state_ = kPrepared;
    return Status::ok_status();
}

Status V4L2System::reset() {
    Status first_failure;
    try {
        if (state_ == kRunning && fd_ >= 0 && api_) {
            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (bounded_ioctl(VIDIOC_STREAMOFF, &type) < 0) {
                remember_first(&first_failure, syscall_failure(
                    "VIDIOC_STREAMOFF", api_->last_error()));
            }
        }
        state_ = kPrepared;

        while (!regions_.empty()) {
            const MappedRegion region = regions_.back();
            regions_.pop_back();
            if (api_ && api_->unmap_memory(region.address, region.length) < 0) {
                remember_first(&first_failure, syscall_failure(
                    "munmap", api_->last_error()));
            }
        }

        if (kernel_buffers_allocated_) {
            struct v4l2_requestbuffers request_buffers;
            std::memset(&request_buffers, 0, sizeof(request_buffers));
            request_buffers.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            request_buffers.memory = V4L2_MEMORY_MMAP;
            if (api_ && fd_ >= 0 &&
                bounded_ioctl(VIDIOC_REQBUFS, &request_buffers) < 0) {
                remember_first(&first_failure, syscall_failure(
                    "VIDIOC_REQBUFS(0)", api_->last_error()));
            }
            kernel_buffers_allocated_ = false;
        }

        if (fd_ >= 0) {
            const int descriptor = fd_;
            fd_ = -1;
            if (api_ && api_->close_device(descriptor) < 0) {
                remember_first(&first_failure, syscall_failure(
                    "close", api_->last_error()));
            }
        }
        negotiated_.reset();
        state_ = kCreated;
        return first_failure;
    } catch (...) {
        cleanup_noexcept();
        if (!first_failure.ok()) return first_failure;
        return v4l2_failure(StatusCode::kInternal,
                            "unexpected failure while resetting V4L2",
                            "reset", 0);
    }
}

void V4L2System::cleanup_noexcept() noexcept {
    if (state_ == kRunning && fd_ >= 0 && api_) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        try {
            bounded_ioctl(VIDIOC_STREAMOFF, &type);
        } catch (...) {
        }
    }
    state_ = kPrepared;
    while (!regions_.empty()) {
        const MappedRegion region = regions_.back();
        regions_.pop_back();
        if (api_) {
            try {
                api_->unmap_memory(region.address, region.length);
            } catch (...) {
            }
        }
    }
    if (kernel_buffers_allocated_) {
        kernel_buffers_allocated_ = false;
        if (api_ && fd_ >= 0) {
            struct v4l2_requestbuffers request_buffers;
            std::memset(&request_buffers, 0, sizeof(request_buffers));
            request_buffers.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            request_buffers.memory = V4L2_MEMORY_MMAP;
            try {
                bounded_ioctl(VIDIOC_REQBUFS, &request_buffers);
            } catch (...) {
            }
        }
    }
    if (fd_ >= 0) {
        const int descriptor = fd_;
        fd_ = -1;
        if (api_) {
            try {
                api_->close_device(descriptor);
            } catch (...) {
            }
        }
    }
    negotiated_.reset();
    state_ = kCreated;
}

Result<std::vector<struct pollfd> > V4L2System::poll_descriptors() const {
    if ((state_ != kPrepared && state_ != kRunning) || fd_ < 0) {
        return Result<std::vector<struct pollfd> >(v4l2_failure(
            StatusCode::kInvalidState,
            "V4L2 poll descriptors require a prepared device",
            "poll_descriptors", 0));
    }
    try {
        const struct pollfd descriptor = {
            fd_, static_cast<short>(POLLIN | POLLRDNORM | POLLPRI), 0};
        return Result<std::vector<struct pollfd> >(
            std::vector<struct pollfd>(1U, descriptor));
    } catch (const std::bad_alloc&) {
        return Result<std::vector<struct pollfd> >(v4l2_failure(
            StatusCode::kResourceExhausted,
            "failed to allocate V4L2 poll descriptors",
            "poll_descriptors", ENOMEM));
    } catch (...) {
        return Result<std::vector<struct pollfd> >(v4l2_failure(
            StatusCode::kInternal,
            "failed to create V4L2 poll descriptors",
            "poll_descriptors", 0));
    }
}

Result<bool> V4L2System::evaluate_poll_events(
    const std::vector<struct pollfd>& descriptors) const {
    if (state_ != kRunning || fd_ < 0 || !api_) {
        return Result<bool>(v4l2_failure(
            StatusCode::kInvalidState,
            "V4L2 poll events require a running device",
            "poll", 0));
    }
    if (descriptors.size() != 1U || descriptors[0].fd != fd_) {
        return Result<bool>(v4l2_failure(
            StatusCode::kInvalidArgument,
            "V4L2 poll descriptor changed before event evaluation",
            "poll", 0));
    }

    const short revents = descriptors[0].revents;
    if ((revents & static_cast<short>(POLLHUP | POLLNVAL)) != 0) {
        return Result<bool>(v4l2_failure(
            StatusCode::kDeviceLost,
            "V4L2 poll descriptor reports device loss",
            "poll", revents));
    }
    if ((revents & POLLERR) != 0) {
        return Result<bool>(v4l2_failure(
            StatusCode::kIoError,
            "V4L2 poll descriptor reports an I/O error",
            "poll", revents));
    }
    return Result<bool>(
        (revents & static_cast<short>(POLLIN | POLLRDNORM | POLLPRI)) != 0);
}

}  // namespace detail
}  // namespace eavp
