#ifndef EAVP_TESTS_SUPPORT_V4L2_TEST_UTILS_HPP_
#define EAVP_TESTS_SUPPORT_V4L2_TEST_UTILS_HPP_

#include <cstddef>

#include "eavp/base/status.hpp"
#include "eavp/media/video_format.hpp"

namespace eavp_test {

struct V4L2ConfigCase {
    const char* name;
    const char* device_path;
    eavp::PixelFormat pixel_format;
    int width;
    int height;
    int frame_rate_numerator;
    int frame_rate_denominator;
    std::size_t buffer_count;
    eavp::StatusCode expected_status;
};

}  // namespace eavp_test

#endif  // EAVP_TESTS_SUPPORT_V4L2_TEST_UTILS_HPP_
