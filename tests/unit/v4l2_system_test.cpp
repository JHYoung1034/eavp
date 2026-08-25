#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <limits>
#include <memory>
#include <poll.h>
#include <string>
#include <sys/mman.h>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "platform/linux/v4l2_system.hpp"
#include "support/fake_v4l2_api.hpp"

namespace {

using eavp_test::FakeV4L2Api;
using eavp_test::FakeV4L2Trace;

eavp::V4L2CaptureConfig make_config(
    eavp::PixelFormat pixel_format = eavp::PixelFormat::kYuv420p,
    int width = 16, int height = 8, int frame_rate_numerator = 30,
    int frame_rate_denominator = 1, std::size_t buffer_count = 3U) {
    eavp::Result<eavp::V4L2CaptureConfig> result =
        eavp::V4L2CaptureConfig::create(
            "/dev/video-test", pixel_format, width, height,
            frame_rate_numerator, frame_rate_denominator, buffer_count);
    EXPECT_TRUE(result.ok());
    return result.take_value();
}

class ScriptedV4L2 {
public:
    ScriptedV4L2()
        : trace(new FakeV4L2Trace()), fake(new FakeV4L2Api(trace)),
          observed(fake.get()),
          system(new eavp::detail::V4L2System(std::move(fake))) {}

    std::shared_ptr<FakeV4L2Trace> trace;
    std::unique_ptr<FakeV4L2Api> fake;
    FakeV4L2Api* observed;
    std::unique_ptr<eavp::detail::V4L2System> system;
};

void expect_provider_context(const eavp::Status& status,
                             const char* operation, int native_code) {
    ASSERT_FALSE(status.ok());
    EXPECT_EQ("v4l2", status.provider_id());
    EXPECT_EQ(operation, status.operation());
    ASSERT_TRUE(status.has_native_code());
    EXPECT_EQ(native_code, status.native_code());
}

void expect_cleanup_tail(const std::vector<std::string>& operations,
                         std::size_t mapped_count, bool kernel_allocation) {
    const std::size_t expected_tail = mapped_count +
        (kernel_allocation ? 1U : 0U) + 1U;
    ASSERT_GE(operations.size(), expected_tail);
    std::size_t position = operations.size() - expected_tail;
    for (std::size_t index = 0U; index < mapped_count; ++index) {
        EXPECT_EQ("munmap", operations[position++]);
    }
    if (kernel_allocation) {
        EXPECT_EQ("VIDIOC_REQBUFS(0)", operations[position++]);
    }
    EXPECT_EQ("close", operations[position]);
}

TEST(V4L2SystemTest, NegotiatesPaddedYuv420pLayoutExactly) {
    ScriptedV4L2 fixture;
    fixture.observed->set_format(V4L2_PIX_FMT_YUV420, 16U, 8U, 32U, 400U);

    ASSERT_TRUE(fixture.system->prepare(make_config()).ok());
    const eavp::detail::V4L2NegotiatedFormat& negotiated =
        fixture.system->negotiated();
    const eavp::VideoFormat& format = negotiated.format;

    EXPECT_EQ(eavp::PixelFormat::kYuv420p, format.pixel_format());
    EXPECT_EQ(eavp::MemoryDomain::kMmap, format.memory_domain());
    ASSERT_EQ(3U, format.planes().size());
    EXPECT_EQ(0U, format.planes()[0].offset);
    EXPECT_EQ(256U, format.planes()[0].size);
    EXPECT_EQ(32U, format.planes()[0].stride);
    EXPECT_EQ(256U, format.planes()[1].offset);
    EXPECT_EQ(64U, format.planes()[1].size);
    EXPECT_EQ(16U, format.planes()[1].stride);
    EXPECT_EQ(320U, format.planes()[2].offset);
    EXPECT_EQ(64U, format.planes()[2].size);
    EXPECT_EQ(16U, format.planes()[2].stride);
    EXPECT_EQ(400U, negotiated.total_capacity);
    const std::size_t visible[] = {16U, 8U, 8U};
    const std::size_t offsets[] = {0U, 256U, 320U};
    ASSERT_EQ(3U, negotiated.visible_row_bytes.size());
    ASSERT_EQ(3U, negotiated.source_offsets.size());
    for (std::size_t index = 0U; index < 3U; ++index) {
        EXPECT_EQ(visible[index], negotiated.visible_row_bytes[index]);
        EXPECT_EQ(offsets[index], negotiated.source_offsets[index]);
    }

    const std::vector<std::string> expected_operations = {
        "open", "VIDIOC_QUERYCAP", "VIDIOC_S_FMT", "VIDIOC_G_FMT",
        "VIDIOC_S_PARM", "VIDIOC_G_PARM", "VIDIOC_REQBUFS",
        "VIDIOC_QUERYBUF", "mmap", "VIDIOC_QUERYBUF", "mmap",
        "VIDIOC_QUERYBUF", "mmap"};
    EXPECT_EQ(expected_operations, fixture.trace->operations);
    EXPECT_EQ("/dev/video-test", fixture.trace->last_open_path);
    EXPECT_EQ(O_RDWR | O_NONBLOCK | O_CLOEXEC,
              fixture.trace->last_open_flags);
    EXPECT_EQ(V4L2_PIX_FMT_YUV420,
              fixture.trace->requested_pixel_format);
    EXPECT_EQ(static_cast<std::uint32_t>(V4L2_BUF_TYPE_VIDEO_CAPTURE),
              fixture.trace->requested_format_type);
    EXPECT_EQ(16U, fixture.trace->requested_width);
    EXPECT_EQ(8U, fixture.trace->requested_height);
    EXPECT_EQ(static_cast<std::uint32_t>(V4L2_FIELD_ANY),
              fixture.trace->requested_field);
    EXPECT_EQ(1U, fixture.trace->requested_frame_numerator);
    EXPECT_EQ(30U, fixture.trace->requested_frame_denominator);
    EXPECT_EQ(static_cast<std::uint32_t>(V4L2_BUF_TYPE_VIDEO_CAPTURE),
              fixture.trace->requested_parameter_type);
    EXPECT_EQ(3U, fixture.trace->requested_buffer_count);
    EXPECT_EQ(static_cast<std::uint32_t>(V4L2_BUF_TYPE_VIDEO_CAPTURE),
              fixture.trace->requested_buffer_type);
    EXPECT_EQ(static_cast<std::uint32_t>(V4L2_MEMORY_MMAP),
              fixture.trace->requested_memory_type);
    ASSERT_EQ(3U, fixture.trace->queried_indices.size());
    ASSERT_EQ(3U, fixture.trace->queried_buffer_types.size());
    ASSERT_EQ(3U, fixture.trace->queried_memory_types.size());
    ASSERT_EQ(3U, fixture.trace->mmap_calls.size());
    for (std::size_t index = 0U; index < 3U; ++index) {
        EXPECT_EQ(index, fixture.trace->queried_indices[index]);
        EXPECT_EQ(static_cast<std::uint32_t>(V4L2_BUF_TYPE_VIDEO_CAPTURE),
                  fixture.trace->queried_buffer_types[index]);
        EXPECT_EQ(static_cast<std::uint32_t>(V4L2_MEMORY_MMAP),
                  fixture.trace->queried_memory_types[index]);
        EXPECT_EQ(NULL, fixture.trace->mmap_calls[index].address);
        EXPECT_EQ(400U, fixture.trace->mmap_calls[index].length);
        EXPECT_EQ(PROT_READ | PROT_WRITE,
                  fixture.trace->mmap_calls[index].protection);
        EXPECT_EQ(MAP_SHARED, fixture.trace->mmap_calls[index].flags);
        EXPECT_EQ(41, fixture.trace->mmap_calls[index].fd);
        EXPECT_EQ(static_cast<std::int64_t>(index * 4096U),
                  fixture.trace->mmap_calls[index].offset);
    }
}

TEST(V4L2SystemTest, NegotiatesPaddedNv12AndYuyvLayoutsExactly) {
    {
        ScriptedV4L2 fixture;
        fixture.observed->set_format(V4L2_PIX_FMT_NV12, 16U, 8U, 32U, 384U);
        ASSERT_TRUE(fixture.system->prepare(
            make_config(eavp::PixelFormat::kNv12)).ok());
        const eavp::detail::V4L2NegotiatedFormat& negotiated =
            fixture.system->negotiated();
        ASSERT_EQ(2U, negotiated.format.planes().size());
        EXPECT_EQ(32U, negotiated.format.planes()[0].stride);
        EXPECT_EQ(256U, negotiated.format.planes()[0].size);
        EXPECT_EQ(256U, negotiated.format.planes()[1].offset);
        EXPECT_EQ(32U, negotiated.format.planes()[1].stride);
        EXPECT_EQ(128U, negotiated.format.planes()[1].size);
        EXPECT_EQ(384U, negotiated.total_capacity);
        ASSERT_EQ(2U, negotiated.visible_row_bytes.size());
        EXPECT_EQ(16U, negotiated.visible_row_bytes[0]);
        EXPECT_EQ(16U, negotiated.visible_row_bytes[1]);
    }
    {
        ScriptedV4L2 fixture;
        fixture.observed->set_format(V4L2_PIX_FMT_YUYV, 16U, 8U, 40U, 320U);
        ASSERT_TRUE(fixture.system->prepare(
            make_config(eavp::PixelFormat::kYuyv422)).ok());
        const eavp::detail::V4L2NegotiatedFormat& negotiated =
            fixture.system->negotiated();
        ASSERT_EQ(1U, negotiated.format.planes().size());
        EXPECT_EQ(0U, negotiated.format.planes()[0].offset);
        EXPECT_EQ(40U, negotiated.format.planes()[0].stride);
        EXPECT_EQ(320U, negotiated.format.planes()[0].size);
        EXPECT_EQ(320U, negotiated.total_capacity);
        ASSERT_EQ(1U, negotiated.visible_row_bytes.size());
        EXPECT_EQ(32U, negotiated.visible_row_bytes[0]);
        ASSERT_EQ(1U, negotiated.source_offsets.size());
        EXPECT_EQ(0U, negotiated.source_offsets[0]);
    }
}

TEST(V4L2SystemTest, RollbackEveryPrepareSyscallFailureInReverseOrder) {
    struct Case {
        const char* operation;
        std::size_t successful_calls_before_failure;
        std::size_t mapped_count;
        bool open_succeeded;
        bool kernel_allocation;
    };
    const Case cases[] = {
        {"open", 0U, 0U, false, false},
        {"VIDIOC_QUERYCAP", 0U, 0U, true, false},
        {"VIDIOC_S_FMT", 0U, 0U, true, false},
        {"VIDIOC_G_FMT", 0U, 0U, true, false},
        {"VIDIOC_S_PARM", 0U, 0U, true, false},
        {"VIDIOC_G_PARM", 0U, 0U, true, false},
        {"VIDIOC_REQBUFS", 0U, 0U, true, false},
        {"VIDIOC_QUERYBUF", 0U, 0U, true, true},
        {"VIDIOC_QUERYBUF", 1U, 1U, true, true},
        {"VIDIOC_QUERYBUF", 2U, 2U, true, true},
        {"mmap", 0U, 0U, true, true},
        {"mmap", 1U, 1U, true, true},
        {"mmap", 2U, 2U, true, true},
    };

    for (std::size_t case_index = 0U;
         case_index < sizeof(cases) / sizeof(cases[0]); ++case_index) {
        ScriptedV4L2 fixture;
        for (std::size_t success = 0U;
             success < cases[case_index].successful_calls_before_failure;
             ++success) {
            fixture.observed->script_error(cases[case_index].operation, 0);
        }
        fixture.observed->script_error(cases[case_index].operation, EIO);

        const eavp::Status status = fixture.system->prepare(make_config());

        expect_provider_context(status, cases[case_index].operation, EIO);
        EXPECT_EQ(cases[case_index].mapped_count,
                  fixture.trace->munmap_calls.size()) << case_index;
        EXPECT_EQ(cases[case_index].kernel_allocation ? 1U : 0U,
                  fixture.trace->request_buffers_zero_calls) << case_index;
        EXPECT_EQ(cases[case_index].open_succeeded ? 1U : 0U,
                  fixture.trace->close_calls) << case_index;
        if (cases[case_index].open_succeeded) {
            expect_cleanup_tail(fixture.trace->operations,
                                cases[case_index].mapped_count,
                                cases[case_index].kernel_allocation);
        }
        for (std::size_t index = 0U;
             index < cases[case_index].mapped_count; ++index) {
            const std::size_t source =
                cases[case_index].mapped_count - index - 1U;
            const std::uintptr_t expected_address =
                static_cast<std::uintptr_t>(0x20000U) +
                source * static_cast<std::uintptr_t>(0x10000U);
            EXPECT_EQ(reinterpret_cast<void*>(expected_address),
                      fixture.trace->munmap_calls[index].address) << case_index;
        }
        EXPECT_TRUE(fixture.system->reset().ok()) << case_index;
        EXPECT_EQ(cases[case_index].open_succeeded ? 1U : 0U,
                  fixture.trace->close_calls) << case_index;
    }
}

TEST(V4L2SystemTest, RejectsMissingSinglePlanarCaptureOrStreamingCapability) {
    const std::uint32_t capabilities[] = {
        V4L2_CAP_STREAMING,
        V4L2_CAP_VIDEO_CAPTURE,
        V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_STREAMING,
    };
    for (std::size_t index = 0U;
         index < sizeof(capabilities) / sizeof(capabilities[0]); ++index) {
        ScriptedV4L2 fixture;
        fixture.observed->set_capabilities(
            V4L2_CAP_DEVICE_CAPS, capabilities[index]);

        const eavp::Status status = fixture.system->prepare(make_config());

        EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch, status.code());
        expect_provider_context(status, "VIDIOC_QUERYCAP", 0);
        EXPECT_EQ(1U, fixture.trace->close_calls);
        EXPECT_EQ(0U, fixture.trace->request_buffers_zero_calls);
    }
}

TEST(V4L2SystemTest, UsesDeviceCapsWhenTheCapabilityMarkerIsPresent) {
    ScriptedV4L2 fixture;
    fixture.observed->set_capabilities(
        V4L2_CAP_DEVICE_CAPS | V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING,
        V4L2_CAP_STREAMING);

    const eavp::Status status = fixture.system->prepare(make_config());

    EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch, status.code());
    expect_provider_context(status, "VIDIOC_QUERYCAP", 0);
}

TEST(V4L2SystemTest, UsesLegacyCapabilitiesWhenDeviceCapsAreNotAdvertised) {
    ScriptedV4L2 fixture;
    fixture.observed->set_capabilities(
        V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING, 0U);

    EXPECT_TRUE(fixture.system->prepare(make_config()).ok());
}

TEST(V4L2SystemTest, RejectsDriverChangesToFormatDimensionsOrFrameRate) {
    enum Mutation {
        kPixelFormat,
        kWidth,
        kHeight,
        kFrameNumerator,
        kFrameDenominator,
    };
    const Mutation mutations[] = {
        kPixelFormat, kWidth, kHeight, kFrameNumerator, kFrameDenominator};
    for (std::size_t index = 0U;
         index < sizeof(mutations) / sizeof(mutations[0]); ++index) {
        ScriptedV4L2 fixture;
        const char* operation = "VIDIOC_G_FMT";
        switch (mutations[index]) {
            case kPixelFormat:
                fixture.observed->set_format(
                    V4L2_PIX_FMT_NV12, 16U, 8U, 32U, 384U);
                break;
            case kWidth:
                fixture.observed->set_format(
                    V4L2_PIX_FMT_YUV420, 18U, 8U, 32U, 432U);
                break;
            case kHeight:
                fixture.observed->set_format(
                    V4L2_PIX_FMT_YUV420, 16U, 10U, 32U, 480U);
                break;
            case kFrameNumerator:
                fixture.observed->set_frame_interval(2U, 30U);
                operation = "VIDIOC_G_PARM";
                break;
            case kFrameDenominator:
                fixture.observed->set_frame_interval(1U, 25U);
                operation = "VIDIOC_G_PARM";
                break;
        }

        const eavp::Status status = fixture.system->prepare(make_config());

        EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch, status.code()) << index;
        expect_provider_context(status, operation, 0);
        EXPECT_EQ(1U, fixture.trace->close_calls) << index;
        EXPECT_EQ(0U, fixture.trace->request_buffers_zero_calls) << index;
    }
}

TEST(V4L2SystemTest, AcceptsEquivalentReducedFrameInterval) {
    ScriptedV4L2 fixture;
    fixture.observed->set_frame_interval(1U, 30U);

    const eavp::Status status = fixture.system->prepare(make_config(
        eavp::PixelFormat::kYuv420p, 16, 8, 60, 2));

    EXPECT_TRUE(status.ok());
    EXPECT_EQ(2U, fixture.trace->requested_frame_numerator);
    EXPECT_EQ(60U, fixture.trace->requested_frame_denominator);
}

TEST(V4L2SystemTest, RejectsZeroActualFrameIntervalComponents) {
    const std::uint32_t intervals[][2] = {
        {0U, 30U},
        {1U, 0U},
        {0U, 0U},
    };
    for (std::size_t index = 0U;
         index < sizeof(intervals) / sizeof(intervals[0]); ++index) {
        ScriptedV4L2 fixture;
        fixture.observed->set_frame_interval(
            intervals[index][0], intervals[index][1]);

        const eavp::Status status = fixture.system->prepare(make_config());

        EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch, status.code())
            << index;
        expect_provider_context(status, "VIDIOC_G_PARM", 0);
        EXPECT_EQ(1U, fixture.trace->close_calls) << index;
    }
}

TEST(V4L2SystemTest, RejectsInvalidPaddingAndSizeImageForEveryFormat) {
    struct Case {
        eavp::PixelFormat format;
        std::uint32_t fourcc;
        std::uint32_t bytes_per_line;
        std::uint32_t size_image;
    };
    const Case cases[] = {
        {eavp::PixelFormat::kYuv420p, V4L2_PIX_FMT_YUV420, 31U, 384U},
        {eavp::PixelFormat::kYuv420p, V4L2_PIX_FMT_YUV420, 32U, 383U},
        {eavp::PixelFormat::kNv12, V4L2_PIX_FMT_NV12, 15U, 384U},
        {eavp::PixelFormat::kNv12, V4L2_PIX_FMT_NV12, 32U, 383U},
        {eavp::PixelFormat::kYuyv422, V4L2_PIX_FMT_YUYV, 31U, 256U},
        {eavp::PixelFormat::kYuyv422, V4L2_PIX_FMT_YUYV, 32U, 255U},
    };
    for (std::size_t index = 0U;
         index < sizeof(cases) / sizeof(cases[0]); ++index) {
        ScriptedV4L2 fixture;
        fixture.observed->set_format(cases[index].fourcc, 16U, 8U,
                                     cases[index].bytes_per_line,
                                     cases[index].size_image);

        const eavp::Status status =
            fixture.system->prepare(make_config(cases[index].format));

        EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch, status.code()) << index;
        expect_provider_context(status, "VIDIOC_G_FMT", 0);
        EXPECT_EQ(1U, fixture.trace->close_calls) << index;
    }
}

TEST(V4L2SystemTest, RejectsFewerThanTwoKernelBuffersAndReleasesAllocation) {
    ScriptedV4L2 fixture;
    fixture.observed->set_returned_buffer_count(1U);

    const eavp::Status status = fixture.system->prepare(make_config());

    EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch, status.code());
    expect_provider_context(status, "VIDIOC_REQBUFS", 0);
    EXPECT_EQ(1U, fixture.trace->request_buffers_zero_calls);
    EXPECT_EQ(1U, fixture.trace->close_calls);
    expect_cleanup_tail(fixture.trace->operations, 0U, true);
}

TEST(V4L2SystemTest, RejectsInvalidRequestBufferTypeOrMemoryAndRollsBack) {
    struct Case {
        std::uint32_t type;
        std::uint32_t memory;
    };
    const Case cases[] = {
        {V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_MEMORY_MMAP},
        {V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_USERPTR},
    };
    for (std::size_t index = 0U;
         index < sizeof(cases) / sizeof(cases[0]); ++index) {
        ScriptedV4L2 fixture;
        fixture.observed->set_returned_buffer_metadata(
            cases[index].type, cases[index].memory);

        const eavp::Status status = fixture.system->prepare(make_config());

        EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch, status.code())
            << index;
        expect_provider_context(status, "VIDIOC_REQBUFS", 0);
        EXPECT_EQ(0U, fixture.trace->mmap_calls.size()) << index;
        EXPECT_EQ(0U, fixture.trace->munmap_calls.size()) << index;
        EXPECT_EQ(1U, fixture.trace->request_buffers_zero_calls) << index;
        EXPECT_EQ(1U, fixture.trace->close_calls) << index;
        expect_cleanup_tail(fixture.trace->operations, 0U, true);
    }
}

TEST(V4L2SystemTest, RejectsInvalidQueryBufferIndexAndLength) {
    struct Case {
        std::uint32_t reported_index;
        std::uint32_t length;
    };
    const Case cases[] = {
        {1U, 384U},
        {0U, 0U},
        {0U, 383U},
    };
    for (std::size_t index = 0U;
         index < sizeof(cases) / sizeof(cases[0]); ++index) {
        ScriptedV4L2 fixture;
        fixture.observed->set_buffer(
            0U, cases[index].reported_index, cases[index].length, 0U);

        const eavp::Status status = fixture.system->prepare(make_config());

        EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch, status.code()) << index;
        expect_provider_context(status, "VIDIOC_QUERYBUF", 0);
        EXPECT_EQ(0U, fixture.trace->mmap_calls.size()) << index;
        EXPECT_EQ(1U, fixture.trace->request_buffers_zero_calls) << index;
        EXPECT_EQ(1U, fixture.trace->close_calls) << index;
    }
}

TEST(V4L2SystemTest, RejectsInvalidQueryBufferTypeMemoryOrOffsetBeforeMmap) {
    struct Case {
        std::uint32_t type;
        std::uint32_t memory;
        std::uint32_t offset;
        std::uint64_t maximum_mappable_offset;
    };
    const Case cases[] = {
        {V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_MEMORY_MMAP, 0U,
         std::numeric_limits<std::uint64_t>::max()},
        {V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_USERPTR, 0U,
         std::numeric_limits<std::uint64_t>::max()},
        {V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP, 4097U, 4096U},
    };
    for (std::size_t index = 0U;
         index < sizeof(cases) / sizeof(cases[0]); ++index) {
        ScriptedV4L2 fixture;
        fixture.observed->set_buffer(
            0U, 0U, 384U, cases[index].offset,
            cases[index].type, cases[index].memory);
        fixture.observed->set_maximum_mappable_offset(
            cases[index].maximum_mappable_offset);

        const eavp::Status status = fixture.system->prepare(make_config());

        EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch, status.code())
            << index;
        expect_provider_context(status, "VIDIOC_QUERYBUF", 0);
        EXPECT_EQ(0U, fixture.trace->mmap_calls.size()) << index;
        EXPECT_EQ(0U, fixture.trace->munmap_calls.size()) << index;
        EXPECT_EQ(1U, fixture.trace->request_buffers_zero_calls) << index;
        EXPECT_EQ(1U, fixture.trace->close_calls) << index;
        expect_cleanup_tail(fixture.trace->operations, 0U, true);
    }
}

TEST(V4L2SystemTest, RetriesOpenAndIoctlOnlyWithinSixtyFourEintrAttempts) {
    {
        ScriptedV4L2 fixture;
        fixture.observed->repeat_error("open", EINTR, 63U);
        ASSERT_TRUE(fixture.system->prepare(make_config()).ok());
        EXPECT_EQ(64U, fixture.trace->open_calls);
    }
    {
        ScriptedV4L2 fixture;
        fixture.observed->repeat_error("open", EINTR, 64U);
        const eavp::Status status = fixture.system->prepare(make_config());
        expect_provider_context(status, "open", EINTR);
        EXPECT_EQ(64U, fixture.trace->open_calls);
        EXPECT_EQ(0U, fixture.trace->close_calls);
    }
    {
        ScriptedV4L2 fixture;
        fixture.observed->repeat_error("VIDIOC_QUERYCAP", EINTR, 63U);
        ASSERT_TRUE(fixture.system->prepare(make_config()).ok());
        std::size_t query_cap_calls = 0U;
        for (std::size_t index = 0U;
             index < fixture.trace->operations.size(); ++index) {
            if (fixture.trace->operations[index] == "VIDIOC_QUERYCAP") {
                ++query_cap_calls;
            }
        }
        EXPECT_EQ(64U, query_cap_calls);
    }
    {
        ScriptedV4L2 fixture;
        fixture.observed->repeat_error("VIDIOC_QUERYCAP", EINTR, 64U);
        const eavp::Status status = fixture.system->prepare(make_config());
        expect_provider_context(status, "VIDIOC_QUERYCAP", EINTR);
        EXPECT_EQ(1U, fixture.trace->close_calls);
    }
}

TEST(V4L2SystemTest, PreparePreservesFirstFailureAcrossCleanupFailures) {
    ScriptedV4L2 fixture;
    fixture.observed->script_error("mmap", EOVERFLOW);
    fixture.observed->script_error("VIDIOC_REQBUFS(0)", EBUSY);
    fixture.observed->script_error("close", EINTR);

    const eavp::Status status = fixture.system->prepare(make_config());

    expect_provider_context(status, "mmap", EOVERFLOW);
    EXPECT_EQ(1U, fixture.trace->request_buffers_zero_calls);
    EXPECT_EQ(1U, fixture.trace->close_calls);
    EXPECT_TRUE(fixture.system->reset().ok());
    EXPECT_EQ(1U, fixture.trace->close_calls);
}

TEST(V4L2SystemTest, StartStopAndPollDescriptorsPreservePreparedResources) {
    ScriptedV4L2 fixture;
    ASSERT_TRUE(fixture.system->prepare(make_config()).ok());

    eavp::Result<std::vector<struct pollfd> > prepared_descriptors =
        fixture.system->poll_descriptors();
    ASSERT_TRUE(prepared_descriptors.ok());
    ASSERT_EQ(1U, prepared_descriptors.value().size());
    EXPECT_EQ(41, prepared_descriptors.value()[0].fd);
    EXPECT_EQ(static_cast<short>(POLLIN | POLLRDNORM | POLLPRI),
              prepared_descriptors.value()[0].events);
    EXPECT_EQ(0, prepared_descriptors.value()[0].revents);

    EXPECT_TRUE(fixture.system->start().ok());
    EXPECT_TRUE(fixture.system->start().ok());
    EXPECT_EQ(1U, fixture.trace->stream_on_calls);
    ASSERT_EQ(1U, fixture.trace->stream_on_types.size());
    EXPECT_EQ(static_cast<std::uint32_t>(V4L2_BUF_TYPE_VIDEO_CAPTURE),
              fixture.trace->stream_on_types[0]);
    EXPECT_TRUE(fixture.system->stop().ok());
    EXPECT_TRUE(fixture.system->stop().ok());
    EXPECT_EQ(1U, fixture.trace->stream_off_calls);
    EXPECT_EQ(0U, fixture.trace->munmap_calls.size());
    EXPECT_EQ(0U, fixture.trace->request_buffers_zero_calls);
    EXPECT_EQ(0U, fixture.trace->close_calls);
    EXPECT_TRUE(fixture.system->start().ok());
    EXPECT_EQ(2U, fixture.trace->stream_on_calls);
    ASSERT_EQ(2U, fixture.trace->stream_on_types.size());
    ASSERT_EQ(1U, fixture.trace->stream_off_types.size());
    EXPECT_EQ(static_cast<std::uint32_t>(V4L2_BUF_TYPE_VIDEO_CAPTURE),
              fixture.trace->stream_off_types[0]);
}

TEST(V4L2SystemTest, QueuesEveryMappedBufferBeforeStreamOn) {
    ScriptedV4L2 fixture;
    fixture.observed->set_returned_buffer_count(4U);
    ASSERT_TRUE(fixture.system->prepare(make_config(
        eavp::PixelFormat::kYuv420p, 16, 8, 30, 1, 4U)).ok());

    ASSERT_TRUE(fixture.system->start().ok());

    const std::vector<std::string> expected = {
        "VIDIOC_QBUF:0", "VIDIOC_QBUF:1", "VIDIOC_QBUF:2",
        "VIDIOC_QBUF:3", "VIDIOC_STREAMON"};
    EXPECT_EQ(expected, fixture.trace->streaming_calls);
    ASSERT_EQ(4U, fixture.trace->queued_buffer_types.size());
    ASSERT_EQ(4U, fixture.trace->queued_memory_types.size());
    for (std::size_t index = 0U; index < 4U; ++index) {
        EXPECT_EQ(static_cast<std::uint32_t>(V4L2_BUF_TYPE_VIDEO_CAPTURE),
                  fixture.trace->queued_buffer_types[index]);
        EXPECT_EQ(static_cast<std::uint32_t>(V4L2_MEMORY_MMAP),
                  fixture.trace->queued_memory_types[index]);
    }
}

TEST(V4L2SystemTest, StartStopsBeforeStreamOnWhenQueueingFails) {
    ScriptedV4L2 fixture;
    ASSERT_TRUE(fixture.system->prepare(make_config()).ok());
    fixture.observed->script_error("VIDIOC_QBUF:1", EIO);

    const eavp::Status status = fixture.system->start();

    EXPECT_EQ(eavp::StatusCode::kIoError, status.code());
    expect_provider_context(status, "VIDIOC_QBUF", EIO);
    const std::vector<std::string> expected = {
        "VIDIOC_QBUF:0", "VIDIOC_QBUF:1"};
    EXPECT_EQ(expected, fixture.trace->streaming_calls);
    EXPECT_EQ(0U, fixture.trace->stream_on_calls);
    EXPECT_TRUE(fixture.system->stop().ok());
    EXPECT_EQ(0U, fixture.trace->stream_off_calls);
}

TEST(V4L2SystemTest, StreamOnFailureLeavesPreparedStateWithoutStreamOff) {
    ScriptedV4L2 fixture;
    ASSERT_TRUE(fixture.system->prepare(make_config()).ok());
    fixture.observed->script_error("VIDIOC_STREAMON", EBUSY);

    const eavp::Status status = fixture.system->start();

    EXPECT_EQ(eavp::StatusCode::kIoError, status.code());
    expect_provider_context(status, "VIDIOC_STREAMON", EBUSY);
    const std::vector<std::string> expected = {
        "VIDIOC_QBUF:0", "VIDIOC_QBUF:1", "VIDIOC_QBUF:2",
        "VIDIOC_STREAMON"};
    EXPECT_EQ(expected, fixture.trace->streaming_calls);
    EXPECT_TRUE(fixture.system->poll_descriptors().ok());

    EXPECT_TRUE(fixture.system->stop().ok());
    EXPECT_EQ(expected, fixture.trace->streaming_calls);
    EXPECT_EQ(0U, fixture.trace->stream_off_calls);
}

TEST(V4L2SystemTest, FailedStreamOffRemainsRunningUntilSuccessfulRetry) {
    ScriptedV4L2 fixture;
    ASSERT_TRUE(fixture.system->prepare(make_config()).ok());
    ASSERT_TRUE(fixture.system->start().ok());
    fixture.trace->streaming_calls.clear();
    fixture.observed->script_error("VIDIOC_STREAMOFF", EPIPE);

    const eavp::Status first = fixture.system->stop();

    EXPECT_EQ(eavp::StatusCode::kIoError, first.code());
    expect_provider_context(first, "VIDIOC_STREAMOFF", EPIPE);
    EXPECT_EQ(std::vector<std::string>(1U, "VIDIOC_STREAMOFF"),
              fixture.trace->streaming_calls);

    EXPECT_TRUE(fixture.system->stop().ok());
    EXPECT_EQ(2U, fixture.trace->stream_off_calls);
    EXPECT_EQ(2U, fixture.trace->streaming_calls.size());
    EXPECT_EQ("VIDIOC_STREAMOFF", fixture.trace->streaming_calls[1]);

    EXPECT_TRUE(fixture.system->stop().ok());
    EXPECT_EQ(2U, fixture.trace->stream_off_calls);
    EXPECT_EQ(2U, fixture.trace->streaming_calls.size());
}

TEST(V4L2SystemTest, RejectsStreamingOperationsBeforeRunningWithoutApiCalls) {
    ScriptedV4L2 fixture;
    const struct pollfd descriptor = {
        41, static_cast<short>(POLLIN | POLLRDNORM | POLLPRI), POLLIN};
    const std::vector<struct pollfd> descriptors(1U, descriptor);

    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              fixture.system->dequeue().status().code());
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              fixture.system->requeue(0U).code());
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              fixture.system->evaluate_poll_events(descriptors).status().code());
    EXPECT_TRUE(fixture.trace->operations.empty());
    EXPECT_TRUE(fixture.trace->streaming_calls.empty());

    ASSERT_TRUE(fixture.system->prepare(make_config()).ok());
    const std::size_t prepared_operations = fixture.trace->operations.size();
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              fixture.system->dequeue().status().code());
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              fixture.system->requeue(0U).code());
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              fixture.system->evaluate_poll_events(descriptors).status().code());
    EXPECT_EQ(prepared_operations, fixture.trace->operations.size());
    EXPECT_TRUE(fixture.trace->streaming_calls.empty());
}

TEST(V4L2SystemTest, DequeuesOneBufferWithMappedMetadataAndRequeuesExplicitly) {
    ScriptedV4L2 fixture;
    ASSERT_TRUE(fixture.system->prepare(make_config()).ok());
    ASSERT_TRUE(fixture.system->start().ok());
    fixture.trace->streaming_calls.clear();
    fixture.observed->script_dequeued_buffer(
        1U, 300U, V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC, 77U, 123L, 456L);
    fixture.observed->script_dequeued_buffer(2U, 200U, 0U, 78U, 124L, 0L);

    eavp::Result<eavp::detail::V4L2DequeuedBuffer> result =
        fixture.system->dequeue();

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(1U, result.value().index);
    EXPECT_EQ(reinterpret_cast<const std::uint8_t*>(0x30000U),
              result.value().data);
    EXPECT_EQ(384U, result.value().mapped_length);
    EXPECT_EQ(300U, result.value().bytesused);
    EXPECT_EQ(static_cast<std::uint32_t>(V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC),
              result.value().flags);
    EXPECT_EQ(77U, result.value().sequence);
    EXPECT_EQ(123, result.value().timestamp.tv_sec);
    EXPECT_EQ(456, result.value().timestamp.tv_usec);
    ASSERT_EQ(1U, fixture.trace->dequeued_buffer_types.size());
    EXPECT_EQ(static_cast<std::uint32_t>(V4L2_BUF_TYPE_VIDEO_CAPTURE),
              fixture.trace->dequeued_buffer_types[0]);
    EXPECT_EQ(static_cast<std::uint32_t>(V4L2_MEMORY_MMAP),
              fixture.trace->dequeued_memory_types[0]);
    EXPECT_EQ(std::vector<std::string>(1U, "VIDIOC_DQBUF"),
              fixture.trace->streaming_calls);

    ASSERT_TRUE(fixture.system->requeue(result.value().index).ok());
    ASSERT_EQ(2U, fixture.trace->streaming_calls.size());
    EXPECT_EQ("VIDIOC_QBUF:1", fixture.trace->streaming_calls[1]);
}

TEST(V4L2SystemTest, MapsWouldBlockAndBoundsInterruptedDequeueAttempts) {
    {
        ScriptedV4L2 fixture;
        ASSERT_TRUE(fixture.system->prepare(make_config()).ok());
        ASSERT_TRUE(fixture.system->start().ok());
        fixture.observed->script_error("VIDIOC_DQBUF", EAGAIN);

        const eavp::Result<eavp::detail::V4L2DequeuedBuffer> result =
            fixture.system->dequeue();

        ASSERT_FALSE(result.ok());
        EXPECT_EQ(eavp::StatusCode::kWouldBlock, result.status().code());
        expect_provider_context(result.status(), "VIDIOC_DQBUF", EAGAIN);
    }
    {
        ScriptedV4L2 fixture;
        ASSERT_TRUE(fixture.system->prepare(make_config()).ok());
        ASSERT_TRUE(fixture.system->start().ok());
        fixture.trace->streaming_calls.clear();
        fixture.observed->repeat_error("VIDIOC_DQBUF", EINTR, 63U);
        fixture.observed->script_dequeued_buffer(0U, 384U, 0U, 1U, 1L, 2L);

        const eavp::Result<eavp::detail::V4L2DequeuedBuffer> result =
            fixture.system->dequeue();

        ASSERT_TRUE(result.ok());
        EXPECT_EQ(64U, static_cast<std::size_t>(std::count(
            fixture.trace->streaming_calls.begin(),
            fixture.trace->streaming_calls.end(), "VIDIOC_DQBUF")));
    }
    {
        ScriptedV4L2 fixture;
        ASSERT_TRUE(fixture.system->prepare(make_config()).ok());
        ASSERT_TRUE(fixture.system->start().ok());
        fixture.trace->streaming_calls.clear();
        fixture.observed->repeat_error("VIDIOC_DQBUF", EINTR, 64U);

        const eavp::Result<eavp::detail::V4L2DequeuedBuffer> result =
            fixture.system->dequeue();

        ASSERT_FALSE(result.ok());
        EXPECT_EQ(eavp::StatusCode::kIoError, result.status().code());
        expect_provider_context(result.status(), "VIDIOC_DQBUF", EINTR);
        EXPECT_EQ(64U, fixture.trace->streaming_calls.size());
    }
}

TEST(V4L2SystemTest, MapsDequeueDeviceAndIoErrorsPrecisely) {
    struct Case {
        int native_code;
        eavp::StatusCode status_code;
    };
    const Case cases[] = {
        {ENODEV, eavp::StatusCode::kDeviceLost},
        {ENXIO, eavp::StatusCode::kDeviceLost},
        {EIO, eavp::StatusCode::kDeviceLost},
        {EINVAL, eavp::StatusCode::kIoError},
    };
    for (std::size_t index = 0U;
         index < sizeof(cases) / sizeof(cases[0]); ++index) {
        ScriptedV4L2 fixture;
        ASSERT_TRUE(fixture.system->prepare(make_config()).ok());
        ASSERT_TRUE(fixture.system->start().ok());
        fixture.observed->script_error("VIDIOC_DQBUF", cases[index].native_code);

        const eavp::Result<eavp::detail::V4L2DequeuedBuffer> result =
            fixture.system->dequeue();

        ASSERT_FALSE(result.ok());
        EXPECT_EQ(cases[index].status_code, result.status().code()) << index;
        expect_provider_context(result.status(), "VIDIOC_DQBUF",
                                cases[index].native_code);
    }
}

TEST(V4L2SystemTest, RejectsDequeuedBufferWithUntrustedIndex) {
    ScriptedV4L2 fixture;
    ASSERT_TRUE(fixture.system->prepare(make_config()).ok());
    ASSERT_TRUE(fixture.system->start().ok());
    fixture.trace->streaming_calls.clear();
    fixture.observed->script_dequeued_buffer(3U, 384U, 0U, 0U, 0L, 0L);

    const eavp::Result<eavp::detail::V4L2DequeuedBuffer> result =
        fixture.system->dequeue();

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(eavp::StatusCode::kCorruptData, result.status().code());
    expect_provider_context(result.status(), "VIDIOC_DQBUF", 0);
    EXPECT_EQ(std::vector<std::string>(1U, "VIDIOC_DQBUF"),
              fixture.trace->streaming_calls);
}

TEST(V4L2SystemTest, ExposesBytesUsedAndErrorFlagForExplicitRequeue) {
    struct Case {
        std::uint32_t bytes_used;
        std::uint32_t flags;
    };
    const Case cases[] = {
        {385U, 0U},
        {384U, V4L2_BUF_FLAG_ERROR},
    };
    for (std::size_t index = 0U;
         index < sizeof(cases) / sizeof(cases[0]); ++index) {
        ScriptedV4L2 fixture;
        ASSERT_TRUE(fixture.system->prepare(make_config()).ok());
        ASSERT_TRUE(fixture.system->start().ok());
        fixture.observed->script_dequeued_buffer(
            0U, cases[index].bytes_used, cases[index].flags, 0U, 0L, 0L);

        eavp::Result<eavp::detail::V4L2DequeuedBuffer> result =
            fixture.system->dequeue();

        ASSERT_TRUE(result.ok());
        EXPECT_EQ(cases[index].bytes_used, result.value().bytesused) << index;
        EXPECT_EQ(cases[index].flags, result.value().flags) << index;
        EXPECT_TRUE(fixture.system->requeue(result.value().index).ok())
            << index;
    }
}

TEST(V4L2SystemTest, ValidatesRequeueIndexAndPreservesQueueFailure) {
    ScriptedV4L2 fixture;
    ASSERT_TRUE(fixture.system->prepare(make_config()).ok());
    ASSERT_TRUE(fixture.system->start().ok());
    fixture.trace->streaming_calls.clear();

    const eavp::Status invalid = fixture.system->requeue(3U);

    EXPECT_EQ(eavp::StatusCode::kInvalidArgument, invalid.code());
    expect_provider_context(invalid, "VIDIOC_QBUF", 0);
    EXPECT_TRUE(fixture.trace->streaming_calls.empty());

    fixture.observed->script_error("VIDIOC_QBUF:1", EIO);
    const eavp::Status failed = fixture.system->requeue(1U);

    EXPECT_EQ(eavp::StatusCode::kIoError, failed.code());
    expect_provider_context(failed, "VIDIOC_QBUF", EIO);
    EXPECT_EQ(std::vector<std::string>(1U, "VIDIOC_QBUF:1"),
              fixture.trace->streaming_calls);
}

TEST(V4L2SystemTest, EvaluatesV4L2ReadinessAndPrioritizesTerminalEvents) {
    ScriptedV4L2 fixture;
    ASSERT_TRUE(fixture.system->prepare(make_config()).ok());
    ASSERT_TRUE(fixture.system->start().ok());
    eavp::Result<std::vector<struct pollfd> > descriptors =
        fixture.system->poll_descriptors();
    ASSERT_TRUE(descriptors.ok());

    const short ready_events[] = {POLLIN, POLLRDNORM, POLLPRI};
    for (std::size_t index = 0U; index < 3U; ++index) {
        descriptors.value()[0].revents = ready_events[index];
        const eavp::Result<bool> result =
            fixture.system->evaluate_poll_events(descriptors.value());
        ASSERT_TRUE(result.ok());
        EXPECT_TRUE(result.value()) << index;
    }
    descriptors.value()[0].revents = POLLOUT;
    eavp::Result<bool> unrelated =
        fixture.system->evaluate_poll_events(descriptors.value());
    ASSERT_TRUE(unrelated.ok());
    EXPECT_FALSE(unrelated.value());

    descriptors.value()[0].revents = static_cast<short>(POLLERR | POLLIN);
    eavp::Result<bool> io_error =
        fixture.system->evaluate_poll_events(descriptors.value());
    ASSERT_FALSE(io_error.ok());
    EXPECT_EQ(eavp::StatusCode::kIoError, io_error.status().code());
    expect_provider_context(io_error.status(), "poll", POLLERR | POLLIN);

    const short lost_events[] = {POLLHUP, POLLNVAL};
    for (std::size_t index = 0U; index < 2U; ++index) {
        descriptors.value()[0].revents = static_cast<short>(
            lost_events[index] | POLLERR | POLLPRI);
        const eavp::Result<bool> lost =
            fixture.system->evaluate_poll_events(descriptors.value());
        ASSERT_FALSE(lost.ok());
        EXPECT_EQ(eavp::StatusCode::kDeviceLost, lost.status().code()) << index;
        expect_provider_context(lost.status(), "poll",
                                lost_events[index] | POLLERR | POLLPRI);
    }
}

TEST(V4L2SystemTest, ResetRunningSessionUsesStrictOrderAndIsIdempotent) {
    ScriptedV4L2 fixture;
    ASSERT_TRUE(fixture.system->prepare(make_config()).ok());
    ASSERT_TRUE(fixture.system->start().ok());

    ASSERT_TRUE(fixture.system->reset().ok());
    ASSERT_TRUE(fixture.system->reset().ok());

    const std::vector<std::string>& operations = fixture.trace->operations;
    ASSERT_GE(operations.size(), 6U);
    const std::size_t tail = operations.size() - 6U;
    EXPECT_EQ("VIDIOC_STREAMOFF", operations[tail]);
    EXPECT_EQ("munmap", operations[tail + 1U]);
    EXPECT_EQ("munmap", operations[tail + 2U]);
    EXPECT_EQ("munmap", operations[tail + 3U]);
    EXPECT_EQ("VIDIOC_REQBUFS(0)", operations[tail + 4U]);
    EXPECT_EQ("close", operations[tail + 5U]);
    ASSERT_EQ(3U, fixture.trace->munmap_calls.size());
    EXPECT_EQ(reinterpret_cast<void*>(0x40000U),
              fixture.trace->munmap_calls[0].address);
    EXPECT_EQ(reinterpret_cast<void*>(0x30000U),
              fixture.trace->munmap_calls[1].address);
    EXPECT_EQ(reinterpret_cast<void*>(0x20000U),
              fixture.trace->munmap_calls[2].address);
    EXPECT_EQ(1U, fixture.trace->stream_off_calls);
    EXPECT_EQ(1U, fixture.trace->request_buffers_zero_calls);
    EXPECT_EQ(1U, fixture.trace->close_calls);
}

TEST(V4L2SystemTest, ResetContinuesCleanupAndReturnsTheFirstError) {
    ScriptedV4L2 fixture;
    ASSERT_TRUE(fixture.system->prepare(make_config()).ok());
    ASSERT_TRUE(fixture.system->start().ok());
    fixture.observed->script_error("VIDIOC_STREAMOFF", EPIPE);
    fixture.observed->script_error("munmap", EFAULT);
    fixture.observed->script_error("VIDIOC_REQBUFS(0)", EBUSY);
    fixture.observed->script_error("close", EINTR);

    const eavp::Status status = fixture.system->reset();

    expect_provider_context(status, "VIDIOC_STREAMOFF", EPIPE);
    EXPECT_EQ(3U, fixture.trace->munmap_calls.size());
    EXPECT_EQ(1U, fixture.trace->request_buffers_zero_calls);
    EXPECT_EQ(1U, fixture.trace->close_calls);
    EXPECT_TRUE(fixture.system->reset().ok());
}

TEST(V4L2SystemTest, ResetPreservesFirstFailureWhenLaterCleanupThrows) {
    ScriptedV4L2 fixture;
    ASSERT_TRUE(fixture.system->prepare(make_config()).ok());
    ASSERT_TRUE(fixture.system->start().ok());
    fixture.observed->script_error("VIDIOC_STREAMOFF", EPIPE);
    fixture.observed->script_throw("munmap");

    const eavp::Status status = fixture.system->reset();

    expect_provider_context(status, "VIDIOC_STREAMOFF", EPIPE);
    EXPECT_EQ(3U, fixture.trace->munmap_calls.size());
    EXPECT_EQ(1U, fixture.trace->request_buffers_zero_calls);
    EXPECT_EQ(1U, fixture.trace->close_calls);
    EXPECT_TRUE(fixture.system->reset().ok());
    EXPECT_EQ(3U, fixture.trace->munmap_calls.size());
    EXPECT_EQ(1U, fixture.trace->request_buffers_zero_calls);
    EXPECT_EQ(1U, fixture.trace->close_calls);
}

TEST(V4L2SystemTest, CloseIsNeverRetriedAfterEintr) {
    ScriptedV4L2 fixture;
    ASSERT_TRUE(fixture.system->prepare(make_config()).ok());
    fixture.observed->script_error("close", EINTR);
    fixture.observed->script_error("close", 0);

    const eavp::Status status = fixture.system->reset();

    expect_provider_context(status, "close", EINTR);
    EXPECT_EQ(1U, fixture.trace->close_calls);
    EXPECT_TRUE(fixture.system->reset().ok());
    EXPECT_EQ(1U, fixture.trace->close_calls);
}

TEST(V4L2SystemTest, DestructorCleansRunningSessionExactlyOnce) {
    std::shared_ptr<FakeV4L2Trace> trace(new FakeV4L2Trace());
    {
        std::unique_ptr<FakeV4L2Api> fake(new FakeV4L2Api(trace));
        eavp::detail::V4L2System system(std::move(fake));
        ASSERT_TRUE(system.prepare(make_config()).ok());
        ASSERT_TRUE(system.start().ok());
    }

    EXPECT_EQ(1U, trace->stream_off_calls);
    EXPECT_EQ(3U, trace->munmap_calls.size());
    EXPECT_EQ(1U, trace->request_buffers_zero_calls);
    EXPECT_EQ(1U, trace->close_calls);
}

TEST(V4L2SystemTest, MoveTransfersExclusiveOwnershipWithoutDoubleCleanup) {
    static_assert(!std::is_copy_constructible<eavp::detail::V4L2System>::value,
                  "V4L2System must not be copy constructible");
    static_assert(!std::is_copy_assignable<eavp::detail::V4L2System>::value,
                  "V4L2System must not be copy assignable");
    static_assert(std::is_nothrow_move_constructible<
                      eavp::detail::V4L2System>::value,
                  "V4L2System move construction must be noexcept");
    static_assert(std::is_nothrow_move_assignable<
                      eavp::detail::V4L2System>::value,
                  "V4L2System move assignment must be noexcept");

    std::shared_ptr<FakeV4L2Trace> trace(new FakeV4L2Trace());
    {
        std::unique_ptr<FakeV4L2Api> fake(new FakeV4L2Api(trace));
        eavp::detail::V4L2System original(std::move(fake));
        ASSERT_TRUE(original.prepare(make_config()).ok());
        eavp::detail::V4L2System moved(std::move(original));
        EXPECT_TRUE(moved.reset().ok());
        EXPECT_TRUE(original.reset().ok());
    }
    EXPECT_EQ(3U, trace->munmap_calls.size());
    EXPECT_EQ(1U, trace->request_buffers_zero_calls);
    EXPECT_EQ(1U, trace->close_calls);

    std::shared_ptr<FakeV4L2Trace> destination_trace(new FakeV4L2Trace());
    std::shared_ptr<FakeV4L2Trace> source_trace(new FakeV4L2Trace());
    {
        std::unique_ptr<FakeV4L2Api> destination_fake(
            new FakeV4L2Api(destination_trace));
        std::unique_ptr<FakeV4L2Api> source_fake(
            new FakeV4L2Api(source_trace));
        eavp::detail::V4L2System destination(std::move(destination_fake));
        eavp::detail::V4L2System source(std::move(source_fake));
        ASSERT_TRUE(destination.prepare(make_config()).ok());
        ASSERT_TRUE(source.prepare(make_config()).ok());

        destination = std::move(source);

        EXPECT_EQ(3U, destination_trace->munmap_calls.size());
        EXPECT_EQ(1U, destination_trace->request_buffers_zero_calls);
        EXPECT_EQ(1U, destination_trace->close_calls);
        EXPECT_TRUE(source.reset().ok());
        EXPECT_TRUE(destination.reset().ok());
    }
    EXPECT_EQ(3U, destination_trace->munmap_calls.size());
    EXPECT_EQ(1U, destination_trace->request_buffers_zero_calls);
    EXPECT_EQ(1U, destination_trace->close_calls);
    EXPECT_EQ(3U, source_trace->munmap_calls.size());
    EXPECT_EQ(1U, source_trace->request_buffers_zero_calls);
    EXPECT_EQ(1U, source_trace->close_calls);
}

TEST(V4L2SystemTest, RejectsLifecycleOperationsInInvalidStates) {
    ScriptedV4L2 fixture;
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              fixture.system->start().code());
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              fixture.system->poll_descriptors().status().code());
    ASSERT_TRUE(fixture.system->prepare(make_config()).ok());
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              fixture.system->prepare(make_config()).code());
}

}  // namespace
