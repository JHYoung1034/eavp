#include "eavp/platform/linux/v4l2_capture.hpp"

#include <cstdint>
#include <limits>
#include <new>

namespace eavp {

namespace {

bool checked_add(std::size_t left, std::size_t right, std::size_t* result) {
    if (left > std::numeric_limits<std::size_t>::max() - right) return false;
    *result = left + right;
    return true;
}

bool checked_multiply(std::size_t left, std::size_t right, std::size_t* result) {
    if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    *result = left * right;
    return true;
}

bool has_representable_layout(PixelFormat pixel_format, int width, int height) {
    const std::size_t width_value = static_cast<std::size_t>(width);
    const std::size_t height_value = static_cast<std::size_t>(height);
    std::size_t luma_size = 0U;
    std::size_t chroma_size = 0U;
    std::size_t chroma_offset = 0U;
    std::size_t total_size = 0U;

    switch (pixel_format) {
        case PixelFormat::kYuv420p:
            if (!checked_multiply(width_value, height_value, &luma_size) ||
                !checked_multiply(width_value / 2U, height_value / 2U, &chroma_size) ||
                !checked_add(luma_size, chroma_size, &chroma_offset) ||
                !checked_add(chroma_offset, chroma_size, &total_size)) {
                return false;
            }
            return total_size > 0U;
        case PixelFormat::kNv12:
            if (!checked_multiply(width_value, height_value, &luma_size) ||
                !checked_multiply(width_value, height_value / 2U, &chroma_size) ||
                !checked_add(luma_size, chroma_size, &total_size)) {
                return false;
            }
            return total_size > 0U;
        case PixelFormat::kYuyv422:
            if (!checked_multiply(width_value, 2U, &luma_size) ||
                !checked_multiply(luma_size, height_value, &total_size)) {
                return false;
            }
            return total_size > 0U;
        case PixelFormat::kUnknown:
        case PixelFormat::kRgb24:
            return false;
    }
    return false;
}

bool is_supported_format(PixelFormat pixel_format) {
    return pixel_format == PixelFormat::kYuv420p || pixel_format == PixelFormat::kNv12 ||
           pixel_format == PixelFormat::kYuyv422;
}

bool has_valid_dimensions(PixelFormat pixel_format, int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (pixel_format == PixelFormat::kYuv420p || pixel_format == PixelFormat::kNv12) {
        return (width % 2) == 0 && (height % 2) == 0;
    }
    return pixel_format != PixelFormat::kYuyv422 || (width % 2) == 0;
}

}  // namespace

Result<V4L2CaptureConfig> V4L2CaptureConfig::create(
    const std::string& device_path, PixelFormat pixel_format,
    int width, int height, int frame_rate_numerator,
    int frame_rate_denominator, std::size_t buffer_count) {
    try {
        if (device_path.empty() || frame_rate_numerator <= 0 ||
            frame_rate_denominator <= 0 || buffer_count < 2U ||
            buffer_count > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            return Result<V4L2CaptureConfig>(Status(
                StatusCode::kInvalidArgument,
                "V4L2 capture configuration values are invalid"));
        }
        if (!is_supported_format(pixel_format)) {
            return Result<V4L2CaptureConfig>(Status(
                StatusCode::kUnsupported, "V4L2 capture pixel format is unsupported"));
        }
        if (!has_valid_dimensions(pixel_format, width, height) ||
            !has_representable_layout(pixel_format, width, height)) {
            return Result<V4L2CaptureConfig>(Status(
                StatusCode::kInvalidArgument,
                "V4L2 capture image layout is invalid or overflows"));
        }
        return Result<V4L2CaptureConfig>(V4L2CaptureConfig(
            device_path, pixel_format, width, height, frame_rate_numerator,
            frame_rate_denominator, buffer_count));
    } catch (const std::bad_alloc&) {
        return Result<V4L2CaptureConfig>(Status(StatusCode::kResourceExhausted));
    } catch (...) {
        return Result<V4L2CaptureConfig>(Status(StatusCode::kInternal));
    }
}

}  // namespace eavp
