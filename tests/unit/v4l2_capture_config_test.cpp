#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "eavp/platform/linux/v4l2_capture.hpp"
#include "support/v4l2_test_utils.hpp"

namespace {

TEST(V4L2CaptureConfigTest, AcceptsApprovedVideo10Shape) {
    const eavp::Result<eavp::V4L2CaptureConfig> result =
        eavp::V4L2CaptureConfig::create(
            "/dev/video10", eavp::PixelFormat::kYuv420p,
            1920, 1080, 30, 1, 4U);

    ASSERT_TRUE(result.ok());
    EXPECT_EQ("/dev/video10", result.value().device_path());
    EXPECT_EQ(eavp::PixelFormat::kYuv420p, result.value().pixel_format());
    EXPECT_EQ(1920, result.value().width());
    EXPECT_EQ(1080, result.value().height());
    EXPECT_EQ(30, result.value().frame_rate_numerator());
    EXPECT_EQ(1, result.value().frame_rate_denominator());
    EXPECT_EQ(4U, result.value().buffer_count());
}

TEST(V4L2CaptureConfigTest, RejectsUnsupportedOrInvalidShapes) {
    const eavp_test::V4L2ConfigCase cases[] = {
        {"empty path", "", eavp::PixelFormat::kYuv420p,
         1920, 1080, 30, 1, 4U, eavp::StatusCode::kInvalidArgument},
        {"unknown format", "/dev/video10", eavp::PixelFormat::kUnknown,
         1920, 1080, 30, 1, 4U, eavp::StatusCode::kUnsupported},
        {"rgb format", "/dev/video10", eavp::PixelFormat::kRgb24,
         1920, 1080, 30, 1, 4U, eavp::StatusCode::kUnsupported},
        {"invalid format enum", "/dev/video10",
         static_cast<eavp::PixelFormat>(99), 1920, 1080, 30, 1, 4U,
         eavp::StatusCode::kUnsupported},
        {"zero width", "/dev/video10", eavp::PixelFormat::kNv12,
         0, 1080, 30, 1, 4U, eavp::StatusCode::kInvalidArgument},
        {"negative height", "/dev/video10", eavp::PixelFormat::kNv12,
         1920, -1, 30, 1, 4U, eavp::StatusCode::kInvalidArgument},
        {"zero frame rate numerator", "/dev/video10", eavp::PixelFormat::kNv12,
         1920, 1080, 0, 1, 4U, eavp::StatusCode::kInvalidArgument},
        {"negative frame rate denominator", "/dev/video10", eavp::PixelFormat::kNv12,
         1920, 1080, 30, -1, 4U, eavp::StatusCode::kInvalidArgument},
        {"yuv420 odd width", "/dev/video10", eavp::PixelFormat::kYuv420p,
         1919, 1080, 30, 1, 4U, eavp::StatusCode::kInvalidArgument},
        {"yuv420 odd height", "/dev/video10", eavp::PixelFormat::kYuv420p,
         1920, 1079, 30, 1, 4U, eavp::StatusCode::kInvalidArgument},
        {"nv12 odd width", "/dev/video10", eavp::PixelFormat::kNv12,
         1919, 1080, 30, 1, 4U, eavp::StatusCode::kInvalidArgument},
        {"nv12 odd height", "/dev/video10", eavp::PixelFormat::kNv12,
         1920, 1079, 30, 1, 4U, eavp::StatusCode::kInvalidArgument},
        {"yuyv odd width", "/dev/video10", eavp::PixelFormat::kYuyv422,
         1919, 1080, 30, 1, 4U, eavp::StatusCode::kInvalidArgument},
        {"insufficient buffers", "/dev/video10", eavp::PixelFormat::kYuyv422,
         1920, 1080, 30, 1, 1U, eavp::StatusCode::kInvalidArgument},
    };

    for (std::size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const eavp_test::V4L2ConfigCase& test_case = cases[index];
        SCOPED_TRACE(test_case.name);
        const eavp::Result<eavp::V4L2CaptureConfig> result =
            eavp::V4L2CaptureConfig::create(
                test_case.device_path, test_case.pixel_format, test_case.width,
                test_case.height, test_case.frame_rate_numerator,
                test_case.frame_rate_denominator, test_case.buffer_count);
        ASSERT_FALSE(result.ok());
        EXPECT_EQ(test_case.expected_status, result.status().code());
    }
}

TEST(V4L2CaptureConfigTest, RejectsBufferCountOutsideKernelRequestRange) {
    const std::size_t maximum_kernel_buffer_count =
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
    ASSERT_EQ(std::numeric_limits<std::size_t>::max() == maximum_kernel_buffer_count,
              sizeof(std::size_t) == sizeof(std::uint32_t));
    if (std::numeric_limits<std::size_t>::max() == maximum_kernel_buffer_count) return;

    const eavp::Result<eavp::V4L2CaptureConfig> result =
        eavp::V4L2CaptureConfig::create(
            "/dev/video10", eavp::PixelFormat::kNv12,
            1920, 1080, 30, 1, maximum_kernel_buffer_count + 1U);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument, result.status().code());
}

TEST(V4L2CaptureConfigTest, RejectsReachableLayoutSizeOverflows) {
    const int largest_even_dimension = std::numeric_limits<int>::max() - 1;
    const int largest_dimensions_with_a_representable_luma_plane = 65534;
    const eavp_test::V4L2ConfigCase cases[] = {
        {"yuv420 luma multiplication", "/dev/video10", eavp::PixelFormat::kYuv420p,
         largest_even_dimension, largest_even_dimension, 30, 1, 4U,
         eavp::StatusCode::kInvalidArgument},
        {"nv12 luma multiplication", "/dev/video10", eavp::PixelFormat::kNv12,
         largest_even_dimension, largest_even_dimension, 30, 1, 4U,
         eavp::StatusCode::kInvalidArgument},
        {"yuv420 chroma offset addition", "/dev/video10",
         eavp::PixelFormat::kYuv420p,
         largest_dimensions_with_a_representable_luma_plane,
         largest_dimensions_with_a_representable_luma_plane, 30, 1, 4U,
         eavp::StatusCode::kInvalidArgument},
        {"nv12 chroma offset addition", "/dev/video10", eavp::PixelFormat::kNv12,
         largest_dimensions_with_a_representable_luma_plane,
         largest_dimensions_with_a_representable_luma_plane, 30, 1, 4U,
         eavp::StatusCode::kInvalidArgument},
        {"yuyv total multiplication", "/dev/video10", eavp::PixelFormat::kYuyv422,
         largest_even_dimension, largest_even_dimension, 30, 1, 4U,
         eavp::StatusCode::kInvalidArgument},
    };

    for (std::size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const eavp_test::V4L2ConfigCase& test_case = cases[index];
        SCOPED_TRACE(test_case.name);
        const eavp::Result<eavp::V4L2CaptureConfig> result =
            eavp::V4L2CaptureConfig::create(
                test_case.device_path, test_case.pixel_format, test_case.width,
                test_case.height, test_case.frame_rate_numerator,
                test_case.frame_rate_denominator, test_case.buffer_count);
        if (sizeof(std::size_t) == sizeof(std::uint32_t)) {
            ASSERT_FALSE(result.ok());
            EXPECT_EQ(test_case.expected_status, result.status().code());
        } else {
            ASSERT_TRUE(result.ok());
        }
    }
}

}  // namespace
