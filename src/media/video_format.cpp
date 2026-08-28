#include "eavp/media/video_format.hpp"

#include <limits>
#include <new>

namespace eavp {

namespace {

Status invalid_format(const char* message) {
    return Status(StatusCode::kInvalidArgument, message);
}

bool is_memory_domain(MemoryDomain memory_domain) {
    switch (memory_domain) {
    case MemoryDomain::kCpu:
    case MemoryDomain::kMmap:
    case MemoryDomain::kDmaBuf:
    case MemoryDomain::kDeviceOpaque:
        return true;
    }
    return false;
}

Status validate_plane(const PlaneLayout& plane, std::size_t minimum_stride, std::size_t rows) {
    if (plane.stride < minimum_stride) {
        return invalid_format("video plane stride is smaller than the pixel format requires");
    }
    if (rows != 0U && plane.stride > std::numeric_limits<std::size_t>::max() / rows) {
        return invalid_format("video plane size overflows");
    }
    if (plane.size < plane.stride * rows) {
        return invalid_format("video plane size is smaller than the pixel format requires");
    }
    return Status::ok_status();
}

Status validate_plane_ranges(const std::vector<PlaneLayout>& planes) {
    for (std::size_t index = 0U; index < planes.size(); ++index) {
        const PlaneLayout& plane = planes[index];
        if (plane.offset > std::numeric_limits<std::size_t>::max() - plane.size) {
            return invalid_format("video plane range overflows");
        }
        const std::size_t plane_end = plane.offset + plane.size;
        for (std::size_t previous = 0U; previous < index; ++previous) {
            const PlaneLayout& other = planes[previous];
            if (other.offset >
                std::numeric_limits<std::size_t>::max() - other.size) {
                return invalid_format("video plane range overflows");
            }
            const std::size_t other_end = other.offset + other.size;
            if (plane.offset < other_end && other.offset < plane_end) {
                return invalid_format("video planes must not overlap");
            }
        }
    }
    return Status::ok_status();
}

Status validate_format(PixelFormat pixel_format, int width, int height,
                       const std::vector<PlaneLayout>& planes) {
    if (width <= 0 || height <= 0) {
        return invalid_format("video format dimensions must be positive");
    }
    const Status range_status = validate_plane_ranges(planes);
    if (!range_status.ok()) {
        return range_status;
    }
    const std::size_t pixel_width = static_cast<std::size_t>(width);
    const std::size_t pixel_height = static_cast<std::size_t>(height);

    switch (pixel_format) {
    case PixelFormat::kYuyv422:
        if (width % 2 != 0) {
            return Status(StatusCode::kUnsupported,
                          "YUYV422 with an odd width is unsupported");
        }
        if (planes.size() != 1U) {
            return invalid_format("YUYV422 requires one plane");
        }
        if (pixel_width > std::numeric_limits<std::size_t>::max() / 2U) {
            return invalid_format("YUYV422 stride overflows");
        }
        return validate_plane(planes[0], pixel_width * 2U, pixel_height);
    case PixelFormat::kRgb24:
        if (planes.size() != 1U) {
            return invalid_format("RGB24 requires one plane");
        }
        if (pixel_width > std::numeric_limits<std::size_t>::max() / 3U) {
            return invalid_format("RGB24 stride overflows");
        }
        return validate_plane(planes[0], pixel_width * 3U, pixel_height);
    case PixelFormat::kNv12:
        if (width % 2 != 0 || height % 2 != 0) {
            return Status(StatusCode::kUnsupported,
                          "NV12 with odd dimensions is unsupported in 0.2");
        }
        if (planes.size() != 2U) {
            return invalid_format("NV12 requires two planes");
        }
        {
            Status status = validate_plane(planes[0], pixel_width, pixel_height);
            if (!status.ok()) {
                return status;
            }
            return validate_plane(planes[1], pixel_width, pixel_height / 2U);
        }
    case PixelFormat::kYuv420p:
        if (width % 2 != 0 || height % 2 != 0) {
            return Status(StatusCode::kUnsupported,
                          "YUV420P with odd dimensions is unsupported in 0.2");
        }
        if (planes.size() != 3U) {
            return invalid_format("YUV420P requires three planes");
        }
        {
            Status status = validate_plane(planes[0], pixel_width, pixel_height);
            if (!status.ok()) {
                return status;
            }
            status = validate_plane(planes[1], pixel_width / 2U, pixel_height / 2U);
            if (!status.ok()) {
                return status;
            }
            return validate_plane(planes[2], pixel_width / 2U, pixel_height / 2U);
        }
    case PixelFormat::kUnknown:
        return invalid_format("video pixel format must be known");
    }
    return invalid_format("video pixel format is invalid");
}

}  // namespace

Result<std::string> pixel_format_name(PixelFormat pixel_format) noexcept {
    try {
        switch (pixel_format) {
            case PixelFormat::kNv12:
                return Result<std::string>(std::string("nv12"));
            case PixelFormat::kYuv420p:
                return Result<std::string>(std::string("yuv420p"));
            case PixelFormat::kRgb24:
                return Result<std::string>(std::string("rgb24"));
            case PixelFormat::kYuyv422:
                return Result<std::string>(std::string("yuyv422"));
            case PixelFormat::kUnknown:
                break;
        }
        return Result<std::string>(Status(
            StatusCode::kInvalidArgument, "pixel format has no stable state name"));
    } catch (const std::bad_alloc&) {
        return Result<std::string>(Status(StatusCode::kResourceExhausted));
    } catch (...) {
        return Result<std::string>(Status(StatusCode::kInternal));
    }
}

Result<VideoFormat> VideoFormat::create(PixelFormat pixel_format, int width, int height,
                                        MemoryDomain memory_domain,
                                        const std::vector<PlaneLayout>& planes,
                                        ColorRange color_range,
                                        ColorPrimaries color_primaries,
                                        TransferCharacteristic transfer,
                                        MatrixCoefficients matrix) {
    if (!is_memory_domain(memory_domain)) {
        return Result<VideoFormat>(invalid_format("video memory domain is invalid"));
    }
    const Status status = validate_format(pixel_format, width, height, planes);
    if (!status.ok()) {
        return Result<VideoFormat>(status);
    }
    try {
        return Result<VideoFormat>(VideoFormat(pixel_format, width, height, memory_domain, planes,
                                                color_range, color_primaries, transfer, matrix));
    } catch (const std::bad_alloc&) {
        return Result<VideoFormat>(
            Status(StatusCode::kResourceExhausted, "failed to create video format metadata"));
    }
}

}  // namespace eavp
