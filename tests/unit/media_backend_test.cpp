#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "eavp/media/backend.hpp"
#include "eavp/media/backend_registry.hpp"
#include "eavp/media/capability.hpp"

namespace {

std::size_t align_up(std::size_t value, std::size_t alignment) {
    return ((value + alignment - 1U) / alignment) * alignment;
}

eavp::VideoFormat make_nv12_format_with_storage(
    int width, int height, std::size_t storage_width,
    std::size_t storage_height, eavp::MemoryDomain memory_domain,
    std::size_t uv_offset_gap = 0U) {
    const std::size_t y_size = storage_width * storage_height;
    const std::vector<eavp::PlaneLayout> planes{
        eavp::PlaneLayout(0U, y_size, storage_width),
        eavp::PlaneLayout(y_size + uv_offset_gap,
                          storage_width * storage_height / 2U,
                          storage_width)};
    eavp::Result<eavp::VideoFormat> result = eavp::VideoFormat::create(
        eavp::PixelFormat::kNv12, width, height, memory_domain, planes);
    EXPECT_TRUE(result.ok());
    return result.take_value();
}

eavp::VideoFormat make_nv12_format(int width, int height,
                                   eavp::MemoryDomain memory_domain) {
    return make_nv12_format_with_storage(
        width, height, align_up(static_cast<std::size_t>(width), 16U),
        align_up(static_cast<std::size_t>(height), 16U), memory_domain);
}

eavp::VideoFormat make_compact_nv12_format(
    int width, int height, eavp::MemoryDomain memory_domain) {
    return make_nv12_format_with_storage(
        width, height, static_cast<std::size_t>(width),
        static_cast<std::size_t>(height), memory_domain);
}

eavp::VideoEncoderConfig make_h264_config(int width, int height,
                                          eavp::CodecProfile profile,
                                          int level_idc = 40) {
    const eavp::TimeBase time_base =
        eavp::TimeBase::create(1, 30).take_value();
    eavp::Result<eavp::VideoEncoderConfig> result =
        eavp::VideoEncoderConfig::create(
            eavp::CodecId::kH264, width, height, 30, 1, time_base,
            2000000, 2500000, 30, 0, eavp::RateControlMode::kCbr, profile,
            level_idc, true);
    EXPECT_TRUE(result.ok());
    return result.take_value();
}

eavp::SelectionPreferences no_preferences() {
    return eavp::SelectionPreferences(std::vector<std::string>(), false, false);
}

eavp::SelectionConstraints automatic_selection() {
    return eavp::SelectionConstraints("");
}

std::vector<eavp::PlaneLayoutConstraint> nv12_layout_constraints(
    std::size_t stride_alignment = 16U) {
    return std::vector<eavp::PlaneLayoutConstraint>{
        eavp::PlaneLayoutConstraint(1, 1, 1U, stride_alignment, 64U, 16U,
                                    64U),
        eavp::PlaneLayoutConstraint(1, 2, 1U, stride_alignment, 64U, 16U,
                                    64U)};
}

eavp::FormatMemoryDomain nv12_dmabuf_format(
    std::size_t stride_alignment = 16U) {
    return eavp::FormatMemoryDomain(eavp::PixelFormat::kNv12,
                                    eavp::MemoryDomain::kDmaBuf,
                                    nv12_layout_constraints(stride_alignment));
}

eavp::VideoEncoderCapability make_h264_capability_for_test(
    bool zero_copy = true, std::size_t stride_alignment = 16U) {
    return eavp::VideoEncoderCapability(
        eavp::DimensionRange(16, 1920, 2, 16),
        eavp::DimensionRange(16, 1088, 2, 16), eavp::CodecId::kH264,
        std::vector<eavp::CodecProfile>{eavp::CodecProfile::kH264Main},
        std::vector<int>{40},
        std::vector<eavp::FormatMemoryDomain>{
            nv12_dmabuf_format(stride_alignment)},
        std::vector<eavp::RateControlMode>{eavp::RateControlMode::kCbr},
        zero_copy);
}

eavp::VideoEncoderRequest make_encoder_request(
    int width, int height, eavp::MemoryDomain memory_domain,
    bool require_zero_copy = false,
    eavp::CodecProfile profile = eavp::CodecProfile::kH264Main,
    int level_idc = 40, std::size_t input_address_alignment = 64U,
    const std::string& required_provider_id = "") {
    return eavp::VideoEncoderRequest(
        make_nv12_format(width, height, memory_domain),
        make_h264_config(width, height, profile, level_idc),
        input_address_alignment, require_zero_copy,
        eavp::SelectionConstraints(required_provider_id), no_preferences());
}

eavp::ProviderCapability make_provider(
    const std::string& id, eavp::ProviderKind kind, bool zero_copy,
    const eavp::Status& availability = eavp::Status::ok_status()) {
    return eavp::ProviderCapability(
        id, "1.2.3", "device-0", availability, kind,
        std::vector<eavp::VideoProcessorCapability>(),
        std::vector<eavp::VideoEncoderCapability>{
            make_h264_capability_for_test(zero_copy)},
        4, std::vector<std::string>{"encoder_sessions=4"});
}

eavp::ProviderCapability make_provider_with_encoder_capability(
    const std::string& id, eavp::ProviderKind kind,
    const eavp::VideoEncoderCapability& capability) {
    return eavp::ProviderCapability(
        id, "1.2.3", "device-0", eavp::Status::ok_status(), kind,
        std::vector<eavp::VideoProcessorCapability>(),
        std::vector<eavp::VideoEncoderCapability>{capability}, 4,
        std::vector<std::string>{"encoder_sessions=4"});
}

eavp::VideoProcessorCapability make_processor_capability_for_test(
    bool zero_copy = true) {
    return eavp::VideoProcessorCapability(
        eavp::DimensionRange(16, 1920, 2, 16),
        eavp::DimensionRange(16, 1088, 2, 16),
        eavp::DimensionRange(16, 1920, 2, 16),
        eavp::DimensionRange(16, 1088, 2, 16),
        std::vector<eavp::FormatMemoryDomain>{nv12_dmabuf_format()},
        std::vector<eavp::FormatMemoryDomain>{nv12_dmabuf_format()},
        std::vector<eavp::VideoProcessingOperation>{
            eavp::VideoProcessingOperation::kScaling},
        zero_copy);
}

eavp::VideoProcessorRequest make_processor_request(
    const eavp::SelectionPreferences& preferences = no_preferences(),
    const std::string& required_provider_id = "") {
    const eavp::VideoProcessorConfig config =
        eavp::VideoProcessorConfig::create(
            make_nv12_format(1920, 1080, eavp::MemoryDomain::kDmaBuf),
            make_nv12_format(1280, 720, eavp::MemoryDomain::kDmaBuf), 0, 0,
            1920, 1080, 0)
            .take_value();
    return eavp::VideoProcessorRequest(
        config,
        std::vector<eavp::VideoProcessingOperation>{
            eavp::VideoProcessingOperation::kScaling},
        64U, 64U, false, eavp::SelectionConstraints(required_provider_id),
        preferences);
}

eavp::VideoEncoderRequest make_encoder_request_with_preferences(
    const eavp::SelectionPreferences& preferences,
    const std::string& required_provider_id = "") {
    return eavp::VideoEncoderRequest(
        make_nv12_format(1920, 1080, eavp::MemoryDomain::kDmaBuf),
        make_h264_config(1920, 1080, eavp::CodecProfile::kH264Main), 64U,
        false, eavp::SelectionConstraints(required_provider_id), preferences);
}

class TestProcessor : public eavp::VideoProcessor {
public:
    TestProcessor() : state_(eavp::BackendState::kCreated) {}

    eavp::BackendState state() const { return state_; }

    eavp::Status configure(const eavp::VideoProcessorConfig&) {
        const eavp::Status affinity = bind_to_current_thread();
        if (!affinity.ok()) {
            return affinity;
        }
        state_ = eavp::BackendState::kConfigured;
        return eavp::Status::ok_status();
    }

    eavp::Status submit(
        const std::shared_ptr<const eavp::VideoFrame>&) {
        const eavp::Status affinity = verify_current_thread();
        if (!affinity.ok()) {
            return affinity;
        }
        state_ = eavp::BackendState::kRunning;
        return eavp::Status::ok_status();
    }

    eavp::Result<std::shared_ptr<const eavp::VideoFrame> > receive() {
        const eavp::Status affinity = verify_current_thread();
        if (!affinity.ok()) {
            return eavp::Result<std::shared_ptr<const eavp::VideoFrame> >(
                affinity);
        }
        return eavp::Result<std::shared_ptr<const eavp::VideoFrame> >(
            eavp::Status(eavp::StatusCode::kWouldBlock, "no test frame"));
    }

    eavp::Status begin_drain() {
        const eavp::Status affinity = verify_current_thread();
        if (!affinity.ok()) {
            return affinity;
        }
        state_ = eavp::BackendState::kDraining;
        return eavp::Status::ok_status();
    }

    eavp::Status reset() {
        const eavp::Status affinity = verify_current_thread();
        if (!affinity.ok()) {
            return affinity;
        }
        state_ = eavp::BackendState::kCreated;
        return eavp::Status::ok_status();
    }

private:
    eavp::BackendState state_;
};

class TestEncoder : public eavp::VideoEncoder {
public:
    explicit TestEncoder(bool* destroyed = NULL)
        : state_(eavp::BackendState::kCreated), destroyed_(destroyed) {}

    ~TestEncoder() {
        if (destroyed_ != NULL) {
            *destroyed_ = true;
        }
    }

    eavp::BackendState state() const { return state_; }

    eavp::Status configure(const eavp::VideoFormat&,
                           const eavp::VideoEncoderConfig&) {
        const eavp::Status affinity = bind_to_current_thread();
        if (!affinity.ok()) {
            return affinity;
        }
        state_ = eavp::BackendState::kConfigured;
        return eavp::Status::ok_status();
    }

    eavp::Status submit(
        const std::shared_ptr<const eavp::VideoFrame>&) {
        const eavp::Status affinity = verify_current_thread();
        if (!affinity.ok()) {
            return affinity;
        }
        state_ = eavp::BackendState::kRunning;
        return eavp::Status::ok_status();
    }

    eavp::Result<std::shared_ptr<const eavp::MediaPacket> > receive() {
        const eavp::Status affinity = verify_current_thread();
        if (!affinity.ok()) {
            return eavp::Result<std::shared_ptr<const eavp::MediaPacket> >(
                affinity);
        }
        return eavp::Result<std::shared_ptr<const eavp::MediaPacket> >(
            eavp::Status(eavp::StatusCode::kWouldBlock, "no test packet"));
    }

    eavp::Status begin_drain() {
        const eavp::Status affinity = verify_current_thread();
        if (!affinity.ok()) {
            return affinity;
        }
        state_ = eavp::BackendState::kDraining;
        return eavp::Status::ok_status();
    }

    eavp::Status reset() {
        const eavp::Status affinity = verify_current_thread();
        if (!affinity.ok()) {
            return affinity;
        }
        state_ = eavp::BackendState::kCreated;
        return eavp::Status::ok_status();
    }

private:
    eavp::BackendState state_;
    bool* destroyed_;
};

class TestBackendProvider : public eavp::MediaBackendProvider {
public:
    TestBackendProvider(const std::string& provider_id, bool available,
                        eavp::ProviderKind kind, bool zero_copy)
        : capability_(
              provider_id, "test-1", "test-device",
              available
                  ? eavp::Status::ok_status()
                  : eavp::Status(eavp::StatusCode::kDeviceLost,
                                 "test device is unavailable"),
              kind,
              std::vector<eavp::VideoProcessorCapability>{
                  make_processor_capability_for_test(zero_copy)},
              std::vector<eavp::VideoEncoderCapability>{
                  make_h264_capability_for_test(zero_copy)},
              2, std::vector<std::string>()) {}

    eavp::Result<eavp::ProviderCapability> probe() const {
        return eavp::Result<eavp::ProviderCapability>(capability_);
    }

    eavp::Result<std::unique_ptr<eavp::VideoProcessor> >
    create_video_processor() const {
        std::unique_ptr<eavp::VideoProcessor> processor(new TestProcessor());
        return eavp::Result<std::unique_ptr<eavp::VideoProcessor> >(
            std::move(processor));
    }

    eavp::Result<std::unique_ptr<eavp::VideoEncoder> > create_video_encoder()
        const {
        std::unique_ptr<eavp::VideoEncoder> encoder(new TestEncoder());
        return eavp::Result<std::unique_ptr<eavp::VideoEncoder> >(
            std::move(encoder));
    }

private:
    eavp::ProviderCapability capability_;
};

class AllocationFailingProbeProvider : public TestBackendProvider {
public:
    AllocationFailingProbeProvider()
        : TestBackendProvider("allocation.failure", true,
                              eavp::ProviderKind::kSoftware, true) {}

    eavp::Result<eavp::ProviderCapability> probe() const {
        throw std::bad_alloc();
    }
};

std::shared_ptr<eavp::MediaBackendProvider> make_test_provider(
    const std::string& provider_id, bool available,
    eavp::ProviderKind kind = eavp::ProviderKind::kSoftware,
    bool zero_copy = true) {
    return std::shared_ptr<eavp::MediaBackendProvider>(
        new TestBackendProvider(provider_id, available, kind, zero_copy));
}

TEST(CapabilityTest, RejectsInvalidRangeAndChecksStepAndAlignmentCapacity) {
    EXPECT_FALSE(eavp::DimensionRange(0, 1920, 0, 16).valid());
    EXPECT_FALSE(eavp::DimensionRange(1920, 16, 2, 16).valid());

    const eavp::DimensionRange range(16, 1088, 2, 16);
    EXPECT_TRUE(range.valid());
    EXPECT_FALSE(range.contains(15));
    EXPECT_FALSE(range.contains(17));
    EXPECT_TRUE(range.contains(1080));
    EXPECT_FALSE(range.contains(1090));
}

TEST(CapabilityTest, RejectsResolutionAlignmentAndMemoryDomainMismatch) {
    const eavp::VideoEncoderCapability capability =
        make_h264_capability_for_test();

    EXPECT_FALSE(eavp::DimensionRange(16, 1920, 2, 16).contains(1919));
    EXPECT_FALSE(capability.supports(make_encoder_request(
        1920, 1080, eavp::MemoryDomain::kCpu)));
    EXPECT_TRUE(capability.supports(make_encoder_request(
        1920, 1080, eavp::MemoryDomain::kDmaBuf)));
}

TEST(CapabilityTest, InvalidDimensionRangeNeverMatchesARequest) {
    const eavp::VideoEncoderCapability capability(
        eavp::DimensionRange(16, 1920, 0, 16),
        eavp::DimensionRange(16, 1088, 2, 16), eavp::CodecId::kH264,
        std::vector<eavp::CodecProfile>{eavp::CodecProfile::kH264Main},
        std::vector<int>{40},
        std::vector<eavp::FormatMemoryDomain>{nv12_dmabuf_format()},
        std::vector<eavp::RateControlMode>{eavp::RateControlMode::kCbr}, true);

    const eavp::Status status = capability.match(make_encoder_request(
        1920, 1080, eavp::MemoryDomain::kDmaBuf));
    EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch, status.code());
    EXPECT_NE(std::string::npos, status.message().find("invalid dimension"));
}

TEST(CapabilityTest, ReturnsExplainableFirstRequiredMismatch) {
    const eavp::VideoEncoderCapability capability =
        make_h264_capability_for_test();
    const eavp::Status status = capability.match(make_encoder_request(
        1920, 1080, eavp::MemoryDomain::kCpu));

    EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch, status.code());
    EXPECT_NE(std::string::npos, status.message().find("memory domain"));
}

TEST(CapabilityTest, RejectsInputAndEncoderConfigDimensionMismatchSeparately) {
    const eavp::VideoEncoderCapability capability =
        make_h264_capability_for_test();
    const eavp::VideoEncoderRequest request(
        make_nv12_format(1920, 1080, eavp::MemoryDomain::kDmaBuf),
        make_h264_config(1918, 1080, eavp::CodecProfile::kH264Main), 64U,
        false, automatic_selection(), no_preferences());

    const eavp::Status status = capability.match(request);
    EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch, status.code());
    EXPECT_NE(std::string::npos, status.message().find("input dimensions"));
}

TEST(CapabilityTest, RejectsUnsupportedCodecProfileLevelAndRateControlCombination) {
    const eavp::VideoEncoderCapability capability =
        make_h264_capability_for_test();

    EXPECT_FALSE(capability.supports(make_encoder_request(
        1920, 1080, eavp::MemoryDomain::kDmaBuf, false,
        eavp::CodecProfile::kH265Main)));
    EXPECT_FALSE(capability.supports(make_encoder_request(
        1920, 1080, eavp::MemoryDomain::kDmaBuf, false,
        eavp::CodecProfile::kH264Main, 41)));

    eavp::Result<eavp::VideoEncoderConfig> vbr =
        eavp::VideoEncoderConfig::create(
            eavp::CodecId::kH264, 1920, 1080, 30, 1,
            eavp::TimeBase::create(1, 30).take_value(), 2000000, 2500000, 30, 0,
            eavp::RateControlMode::kVbr, eavp::CodecProfile::kH264Main, 40,
            true);
    ASSERT_TRUE(vbr.ok());
    EXPECT_FALSE(capability.supports(eavp::VideoEncoderRequest(
        make_nv12_format(1920, 1080, eavp::MemoryDomain::kDmaBuf),
        vbr.take_value(), 64U, false, automatic_selection(),
        no_preferences())));
}

TEST(CapabilityTest, RequiredZeroCopyRejectsButPreferenceDoesNot) {
    const eavp::VideoEncoderCapability software_capability =
        make_h264_capability_for_test(false);

    EXPECT_FALSE(software_capability.supports(make_encoder_request(
        1920, 1080, eavp::MemoryDomain::kDmaBuf, true)));

    eavp::VideoEncoderRequest preferred_request(
        make_nv12_format(1920, 1080, eavp::MemoryDomain::kDmaBuf),
        make_h264_config(1920, 1080, eavp::CodecProfile::kH264Main), 64U,
        false, automatic_selection(),
        eavp::SelectionPreferences(std::vector<std::string>(), true, true));
    EXPECT_TRUE(software_capability.supports(preferred_request));
}

TEST(CapabilityTest, RequiredZeroCopyValidatesPlaneExtentStrideOffsetAndAddress) {
    const eavp::VideoEncoderCapability capability =
        make_h264_capability_for_test(true);
    const eavp::VideoEncoderConfig config =
        make_h264_config(1920, 1080, eavp::CodecProfile::kH264Main);
    const eavp::VideoFormat compact = make_compact_nv12_format(
        1920, 1080, eavp::MemoryDomain::kDmaBuf);
    const eavp::VideoFormat wrong_stride = make_nv12_format_with_storage(
        1920, 1080, 1922U, 1088U, eavp::MemoryDomain::kDmaBuf);
    const eavp::VideoFormat wrong_offset = make_nv12_format_with_storage(
        1920, 1080, 1920U, 1088U, eavp::MemoryDomain::kDmaBuf, 1U);

    EXPECT_FALSE(capability.supports(eavp::VideoEncoderRequest(
        compact, config, 64U, true, automatic_selection(), no_preferences())));
    EXPECT_FALSE(capability.supports(eavp::VideoEncoderRequest(
        wrong_stride, config, 64U, true, automatic_selection(),
        no_preferences())));
    EXPECT_FALSE(capability.supports(eavp::VideoEncoderRequest(
        wrong_offset, config, 64U, true, automatic_selection(),
        no_preferences())));
    EXPECT_FALSE(capability.supports(eavp::VideoEncoderRequest(
        make_nv12_format(1920, 1080, eavp::MemoryDomain::kDmaBuf), config,
        0U, true, automatic_selection(), no_preferences())));
    EXPECT_TRUE(capability.supports(eavp::VideoEncoderRequest(
        make_nv12_format(1920, 1080, eavp::MemoryDomain::kDmaBuf), config,
        64U, true, automatic_selection(), no_preferences())));
}

TEST(CapabilityTest, RejectsOverlappingAlignedPlaneRangesForRequiredZeroCopy) {
    const std::size_t y_size = 1920U * 1088U;
    const std::vector<eavp::PlaneLayout> overlapping_planes{
        eavp::PlaneLayout(0U, y_size, 1920U),
        eavp::PlaneLayout(64U, 1920U * 544U, 1920U)};
    const eavp::Result<eavp::VideoFormat> format =
        eavp::VideoFormat::create(eavp::PixelFormat::kNv12, 1920, 1080,
                                  eavp::MemoryDomain::kDmaBuf,
                                  overlapping_planes);
    ASSERT_TRUE(format.ok());
    const eavp::VideoEncoderRequest request(
        format.value(),
        make_h264_config(1920, 1080, eavp::CodecProfile::kH264Main), 64U,
        true, automatic_selection(), no_preferences());

    EXPECT_FALSE(make_h264_capability_for_test(true).supports(request));
}

TEST(CapabilityTest, RejectsPlaneRangeWhoseOffsetAndSizeOverflow) {
    const std::size_t y_size = 1920U * 1088U;
    const std::size_t overflowing_offset =
        std::numeric_limits<std::size_t>::max() - 63U;
    const std::vector<eavp::PlaneLayout> overflowing_planes{
        eavp::PlaneLayout(0U, y_size, 1920U),
        eavp::PlaneLayout(overflowing_offset, 1920U * 544U, 1920U)};
    const eavp::Result<eavp::VideoFormat> format =
        eavp::VideoFormat::create(eavp::PixelFormat::kNv12, 1920, 1080,
                                  eavp::MemoryDomain::kDmaBuf,
                                  overflowing_planes);
    ASSERT_TRUE(format.ok());
    const eavp::VideoEncoderRequest request(
        format.value(),
        make_h264_config(1920, 1080, eavp::CodecProfile::kH264Main), 64U,
        true, automatic_selection(), no_preferences());

    EXPECT_FALSE(make_h264_capability_for_test(true).supports(request));

    const eavp::VideoEncoderRequest conversion_request(
        format.value(),
        make_h264_config(1920, 1080, eavp::CodecProfile::kH264Main), 64U,
        false, automatic_selection(), no_preferences());
    const eavp::Result<eavp::VideoEncoderNegotiation> negotiation =
        make_provider("overflow.safe", eavp::ProviderKind::kHardware, true)
            .negotiate(conversion_request);
    ASSERT_TRUE(negotiation.ok());
    EXPECT_TRUE(negotiation.value().requires_explicit_conversion);
    const std::vector<eavp::PlaneLayout>& actual_planes =
        negotiation.value().actual_format.planes();
    ASSERT_EQ(2U, actual_planes.size());
    ASSERT_LE(actual_planes[0].offset,
              std::numeric_limits<std::size_t>::max() - actual_planes[0].size);
    ASSERT_LE(actual_planes[1].offset,
              std::numeric_limits<std::size_t>::max() - actual_planes[1].size);
    ASSERT_LE(actual_planes[0].offset, actual_planes[1].offset);
    EXPECT_LE(actual_planes[0].size,
              actual_planes[1].offset - actual_planes[0].offset);
}

TEST(CapabilityTest, NegotiationReportsExplicitPlaneLayoutConversion) {
    const eavp::ProviderCapability provider = make_provider(
        "hardware.layout", eavp::ProviderKind::kHardware, true);
    const eavp::VideoEncoderRequest request(
        make_compact_nv12_format(1920, 1080, eavp::MemoryDomain::kDmaBuf),
        make_h264_config(1920, 1080, eavp::CodecProfile::kH264Main), 64U,
        false, automatic_selection(), no_preferences());

    const eavp::Result<eavp::VideoEncoderNegotiation> result =
        provider.negotiate(request);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result.value().requires_explicit_conversion);
    EXPECT_FALSE(result.value().zero_copy);
    ASSERT_EQ(2U, result.value().actual_format.planes().size());
    EXPECT_EQ(1920U, result.value().actual_format.planes()[0].stride);
    EXPECT_EQ(1920U * 1088U, result.value().actual_format.planes()[0].size);
    EXPECT_EQ(1920U * 1088U,
              result.value().actual_format.planes()[1].offset);
}

TEST(CapabilityTest, ProcessorRequiresDeclaredOperationsAndExactDomains) {
    const eavp::VideoFormat input =
        make_nv12_format(1920, 1080, eavp::MemoryDomain::kDmaBuf);
    const eavp::VideoFormat output =
        make_nv12_format(1280, 720, eavp::MemoryDomain::kDmaBuf);
    eavp::Result<eavp::VideoProcessorConfig> config =
        eavp::VideoProcessorConfig::create(input, output, 0, 0, 1920, 1080,
                                           0);
    ASSERT_TRUE(config.ok());
    const eavp::VideoProcessorRequest request(
        config.take_value(),
        std::vector<eavp::VideoProcessingOperation>{
            eavp::VideoProcessingOperation::kScaling},
        64U, 64U, false, automatic_selection(), no_preferences());
    const eavp::VideoProcessorCapability capability(
        eavp::DimensionRange(16, 1920, 2, 16),
        eavp::DimensionRange(16, 1088, 2, 16),
        eavp::DimensionRange(16, 1920, 2, 16),
        eavp::DimensionRange(16, 1088, 2, 16),
        std::vector<eavp::FormatMemoryDomain>{nv12_dmabuf_format()},
        std::vector<eavp::FormatMemoryDomain>{nv12_dmabuf_format()},
        std::vector<eavp::VideoProcessingOperation>(), true);

    const eavp::Status status = capability.match(request);
    EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch, status.code());
    EXPECT_NE(std::string::npos, status.message().find("scaling"));
}

TEST(CapabilityTest, ProcessorLayoutMismatchRejectsZeroCopyOrReportsConversion) {
    const eavp::VideoFormat input = make_compact_nv12_format(
        1920, 1080, eavp::MemoryDomain::kDmaBuf);
    const eavp::VideoFormat output =
        make_nv12_format(1280, 720, eavp::MemoryDomain::kDmaBuf);
    const eavp::VideoProcessorConfig config =
        eavp::VideoProcessorConfig::create(input, output, 0, 0, 1920, 1080,
                                           0)
            .take_value();
    const eavp::VideoProcessorCapability capability(
        eavp::DimensionRange(16, 1920, 2, 16),
        eavp::DimensionRange(16, 1088, 2, 16),
        eavp::DimensionRange(16, 1920, 2, 16),
        eavp::DimensionRange(16, 1088, 2, 16),
        std::vector<eavp::FormatMemoryDomain>{nv12_dmabuf_format()},
        std::vector<eavp::FormatMemoryDomain>{nv12_dmabuf_format()},
        std::vector<eavp::VideoProcessingOperation>{
            eavp::VideoProcessingOperation::kScaling},
        true);
    const eavp::VideoProcessorRequest zero_copy_request(
        config,
        std::vector<eavp::VideoProcessingOperation>{
            eavp::VideoProcessingOperation::kScaling},
        64U, 64U, true, automatic_selection(), no_preferences());
    EXPECT_FALSE(capability.supports(zero_copy_request));

    const eavp::VideoProcessorRequest conversion_request(
        config,
        std::vector<eavp::VideoProcessingOperation>{
            eavp::VideoProcessingOperation::kScaling},
        64U, 64U, false, automatic_selection(), no_preferences());
    const eavp::ProviderCapability provider(
        "processor.layout", "1.0", "device-1", eavp::Status::ok_status(),
        eavp::ProviderKind::kHardware,
        std::vector<eavp::VideoProcessorCapability>{capability},
        std::vector<eavp::VideoEncoderCapability>(), 2,
        std::vector<std::string>{"scalers=2"});
    const eavp::Result<eavp::VideoProcessorNegotiation> result =
        provider.negotiate(conversion_request);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result.value().requires_explicit_conversion);
    EXPECT_FALSE(result.value().zero_copy);
    EXPECT_EQ(1920U * 1088U,
              result.value().config.input_format.planes()[0].size);
}

TEST(CapabilityTest, RotationByNinetyDegreesDoesNotImplyScaling) {
    const eavp::VideoFormat input =
        make_nv12_format(1920, 1080, eavp::MemoryDomain::kDmaBuf);
    const eavp::VideoFormat output =
        make_nv12_format(1080, 1920, eavp::MemoryDomain::kDmaBuf);
    const eavp::VideoProcessorConfig config =
        eavp::VideoProcessorConfig::create(input, output, 0, 0, 1920, 1080,
                                           90)
            .take_value();
    const eavp::VideoProcessorRequest request(
        config,
        std::vector<eavp::VideoProcessingOperation>{
            eavp::VideoProcessingOperation::kRotation},
        64U, 64U, true, automatic_selection(), no_preferences());
    const eavp::VideoProcessorCapability capability(
        eavp::DimensionRange(16, 1920, 2, 16),
        eavp::DimensionRange(16, 1088, 2, 16),
        eavp::DimensionRange(16, 1920, 2, 16),
        eavp::DimensionRange(16, 1920, 2, 16),
        std::vector<eavp::FormatMemoryDomain>{nv12_dmabuf_format()},
        std::vector<eavp::FormatMemoryDomain>{nv12_dmabuf_format()},
        std::vector<eavp::VideoProcessingOperation>{
            eavp::VideoProcessingOperation::kRotation},
        true);

    EXPECT_TRUE(capability.supports(request));
}

TEST(CapabilityTest, ProviderPreservesIdentityAvailabilityAndResourceLimits) {
    const eavp::Status unavailable(eavp::StatusCode::kDeviceLost,
                                   "device disappeared");
    const eavp::ProviderCapability provider = make_provider(
        "vendor.encoder", eavp::ProviderKind::kHardware, true, unavailable);

    EXPECT_EQ("vendor.encoder", provider.provider_id());
    EXPECT_EQ("1.2.3", provider.implementation_version());
    EXPECT_EQ("device-0", provider.device_id());
    EXPECT_FALSE(provider.available());
    EXPECT_EQ(eavp::StatusCode::kDeviceLost,
              provider.availability_status().code());
    EXPECT_EQ(eavp::ProviderKind::kHardware, provider.kind());
    EXPECT_EQ(4, provider.max_concurrent_instances());
    ASSERT_EQ(1U, provider.resource_constraints().size());
    EXPECT_EQ("encoder_sessions=4", provider.resource_constraints()[0]);
}

TEST(CapabilityTest, ProviderMatchPreservesUnavailableStatus) {
    const eavp::ProviderCapability provider = make_provider(
        "gone", eavp::ProviderKind::kHardware, true,
        eavp::Status(eavp::StatusCode::kDeviceLost, "device disappeared"));

    const eavp::Result<eavp::VideoEncoderNegotiation> result =
        provider.negotiate(make_encoder_request(
            1920, 1080, eavp::MemoryDomain::kDmaBuf));
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(eavp::StatusCode::kDeviceLost, result.status().code());
}

TEST(CapabilityTest, RequiredProviderIsNotARecoverablePreference) {
    const eavp::ProviderCapability hardware = make_provider(
        "hardware.a", eavp::ProviderKind::kHardware, true);
    const eavp::ProviderCapability software = make_provider(
        "software.b", eavp::ProviderKind::kSoftware, true);
    const eavp::VideoEncoderRequest request = make_encoder_request(
        1920, 1080, eavp::MemoryDomain::kDmaBuf, false,
        eavp::CodecProfile::kH264Main, 40, 64U, "hardware.a");

    EXPECT_TRUE(hardware.supports(request));
    const eavp::Status status = software.match(request);
    EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch, status.code());
    EXPECT_NE(std::string::npos, status.message().find("required provider"));
}

TEST(CapabilityTest, NegotiationReturnsProviderConfigActualFormatAndConversionFlag) {
    const eavp::ProviderCapability provider = make_provider(
        "hardware.a", eavp::ProviderKind::kHardware, true);
    const eavp::Result<eavp::VideoEncoderNegotiation> result =
        provider.negotiate(make_encoder_request(
            1920, 1080, eavp::MemoryDomain::kDmaBuf));

    ASSERT_TRUE(result.ok());
    EXPECT_EQ("hardware.a", result.value().provider_id);
    EXPECT_EQ(1920, result.value().config.width);
    EXPECT_EQ(1080, result.value().actual_format.height());
    EXPECT_EQ(eavp::MemoryDomain::kDmaBuf,
              result.value().actual_format.memory_domain());
    EXPECT_FALSE(result.value().requires_explicit_conversion);
}

TEST(CapabilityTest, ProcessorNegotiationReturnsTypedResultWithoutChangingRequiredConfig) {
    const eavp::VideoFormat input =
        make_nv12_format(1920, 1080, eavp::MemoryDomain::kDmaBuf);
    const eavp::VideoFormat output =
        make_nv12_format(1280, 720, eavp::MemoryDomain::kDmaBuf);
    const eavp::VideoProcessorConfig config =
        eavp::VideoProcessorConfig::create(input, output, 0, 0, 1920, 1080,
                                           0)
            .take_value();
    const eavp::VideoProcessorRequest request(
        config,
        std::vector<eavp::VideoProcessingOperation>{
            eavp::VideoProcessingOperation::kScaling},
        64U, 64U, true, automatic_selection(), no_preferences());
    const eavp::VideoProcessorCapability processor_capability(
        eavp::DimensionRange(16, 1920, 2, 16),
        eavp::DimensionRange(16, 1088, 2, 16),
        eavp::DimensionRange(16, 1920, 2, 16),
        eavp::DimensionRange(16, 1088, 2, 16),
        std::vector<eavp::FormatMemoryDomain>{nv12_dmabuf_format()},
        std::vector<eavp::FormatMemoryDomain>{nv12_dmabuf_format()},
        std::vector<eavp::VideoProcessingOperation>{
            eavp::VideoProcessingOperation::kScaling},
        true);
    const eavp::ProviderCapability provider(
        "processor.a", "1.0", "device-1", eavp::Status::ok_status(),
        eavp::ProviderKind::kHardware,
        std::vector<eavp::VideoProcessorCapability>{processor_capability},
        std::vector<eavp::VideoEncoderCapability>(), 2,
        std::vector<std::string>{"scalers=2"});

    const eavp::Result<eavp::VideoProcessorNegotiation> result =
        provider.negotiate(request);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ("processor.a", result.value().provider_id);
    EXPECT_EQ(1280, result.value().config.output_format.width());
    EXPECT_EQ(1280, result.value().actual_format.width());
    EXPECT_EQ(eavp::MemoryDomain::kDmaBuf,
              result.value().actual_format.memory_domain());
    EXPECT_FALSE(result.value().requires_explicit_conversion);
}

TEST(CapabilityTest, PreferenceScoreIsDeterministicAndDoesNotAffectSupport) {
    const eavp::ProviderCapability software = make_provider(
        "software.z", eavp::ProviderKind::kSoftware, false);
    const eavp::ProviderCapability hardware = make_provider(
        "hardware.a", eavp::ProviderKind::kHardware, true);
    const eavp::SelectionPreferences preferences(
        std::vector<std::string>{"software.z"}, true, true);
    const eavp::VideoEncoderRequest request(
        make_nv12_format(1920, 1080, eavp::MemoryDomain::kDmaBuf),
        make_h264_config(1920, 1080, eavp::CodecProfile::kH264Main), 64U,
        false, automatic_selection(), preferences);

    EXPECT_TRUE(software.supports(request));
    EXPECT_TRUE(hardware.supports(request));
    ASSERT_TRUE(software.preference_score(request).ok());
    ASSERT_TRUE(hardware.preference_score(request).ok());
    EXPECT_LT(software.preference_score(request).value(),
              hardware.preference_score(request).value());

    const eavp::SelectionPreferences no_provider_rank(
        std::vector<std::string>(), true, true);
    const eavp::VideoEncoderRequest no_rank_request(
        make_nv12_format(1920, 1080, eavp::MemoryDomain::kDmaBuf),
        make_h264_config(1920, 1080, eavp::CodecProfile::kH264Main), 64U,
        false, automatic_selection(), no_provider_rank);
    EXPECT_LT(hardware.preference_score(no_rank_request).value(),
              software.preference_score(no_rank_request).value());

    const eavp::ProviderCapability provider_a = make_provider(
        "provider.a", eavp::ProviderKind::kSoftware, false);
    const eavp::ProviderCapability provider_b = make_provider(
        "provider.b", eavp::ProviderKind::kSoftware, false);
    EXPECT_LT(provider_a.preference_score(no_rank_request).value(),
              provider_b.preference_score(no_rank_request).value());
}

TEST(CapabilityTest, ZeroCopyScoreComesFromMatchedLayoutNotCallerInput) {
    const eavp::ProviderCapability needs_copy =
        make_provider_with_encoder_capability(
            "a.needs-copy", eavp::ProviderKind::kHardware,
            make_h264_capability_for_test(true, 2048U));
    const eavp::ProviderCapability zero_copy =
        make_provider_with_encoder_capability(
            "z.zero-copy", eavp::ProviderKind::kHardware,
            make_h264_capability_for_test(true, 16U));
    const eavp::VideoEncoderRequest request(
        make_nv12_format(1920, 1080, eavp::MemoryDomain::kDmaBuf),
        make_h264_config(1920, 1080, eavp::CodecProfile::kH264Main), 64U,
        false, automatic_selection(),
        eavp::SelectionPreferences(std::vector<std::string>(), false, true));

    ASSERT_TRUE(needs_copy.preference_score(request).ok());
    ASSERT_TRUE(zero_copy.preference_score(request).ok());
    EXPECT_LT(zero_copy.preference_score(request).value(),
              needs_copy.preference_score(request).value());
}

TEST(BackendRegistryTest, RejectsDuplicateAndRegistrationAfterFreeze) {
    eavp::BackendRegistry registry;
    std::shared_ptr<eavp::MediaBackendProvider> first =
        make_test_provider("same", true);
    std::shared_ptr<eavp::MediaBackendProvider> second =
        make_test_provider("same", true);

    ASSERT_TRUE(registry.register_provider(first).ok());
    EXPECT_EQ(eavp::StatusCode::kAlreadyExists,
              registry.register_provider(second).code());
    ASSERT_TRUE(registry.freeze().ok());
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              registry.register_provider(make_test_provider("late", true))
                  .code());
}

TEST(BackendRegistryTest, ConvertsProbeAllocationFailureToResourceExhausted) {
    eavp::BackendRegistry registry;
    const std::shared_ptr<eavp::MediaBackendProvider> provider(
        new AllocationFailingProbeProvider());

    const eavp::Status status = registry.register_provider(provider);

    EXPECT_EQ(eavp::StatusCode::kResourceExhausted, status.code());
    EXPECT_TRUE(status.message().empty());
}

TEST(BackendRegistryTest, SelectionIsIndependentOfRegistrationOrder) {
    eavp::BackendRegistry forward;
    ASSERT_TRUE(
        forward.register_provider(make_test_provider("provider.a", true)).ok());
    ASSERT_TRUE(
        forward.register_provider(make_test_provider("provider.z", true)).ok());
    ASSERT_TRUE(forward.freeze().ok());

    eavp::BackendRegistry reverse;
    ASSERT_TRUE(
        reverse.register_provider(make_test_provider("provider.z", true)).ok());
    ASSERT_TRUE(
        reverse.register_provider(make_test_provider("provider.a", true)).ok());
    ASSERT_TRUE(reverse.freeze().ok());

    const eavp::VideoEncoderRequest encoder_request =
        make_encoder_request_with_preferences(no_preferences());
    const eavp::Result<eavp::EncoderSelection> forward_encoder =
        forward.select_video_encoder(encoder_request);
    const eavp::Result<eavp::EncoderSelection> reverse_encoder =
        reverse.select_video_encoder(encoder_request);
    ASSERT_TRUE(forward_encoder.ok());
    ASSERT_TRUE(reverse_encoder.ok());
    EXPECT_EQ("provider.a", forward_encoder.value().negotiation.provider_id);
    EXPECT_EQ(forward_encoder.value().negotiation.provider_id,
              reverse_encoder.value().negotiation.provider_id);

    const eavp::VideoProcessorRequest processor_request =
        make_processor_request();
    const eavp::Result<eavp::ProcessorSelection> forward_processor =
        forward.select_video_processor(processor_request);
    const eavp::Result<eavp::ProcessorSelection> reverse_processor =
        reverse.select_video_processor(processor_request);
    ASSERT_TRUE(forward_processor.ok());
    ASSERT_TRUE(reverse_processor.ok());
    EXPECT_EQ("provider.a", forward_processor.value().negotiation.provider_id);
    EXPECT_EQ(forward_processor.value().negotiation.provider_id,
              reverse_processor.value().negotiation.provider_id);
}

TEST(BackendRegistryTest, UsesCapabilityPreferenceScoreInDocumentedOrder) {
    eavp::BackendRegistry registry;
    ASSERT_TRUE(registry
                    .register_provider(make_test_provider(
                        "z.preferred", true, eavp::ProviderKind::kSoftware,
                        false))
                    .ok());
    ASSERT_TRUE(registry
                    .register_provider(make_test_provider(
                        "b.hardware-copy", true,
                        eavp::ProviderKind::kHardware, false))
                    .ok());
    ASSERT_TRUE(registry
                    .register_provider(make_test_provider(
                        "c.hardware-zero", true,
                        eavp::ProviderKind::kHardware, true))
                    .ok());
    ASSERT_TRUE(registry
                    .register_provider(make_test_provider(
                        "a.reference", true,
                        eavp::ProviderKind::kReference, true))
                    .ok());
    ASSERT_TRUE(registry.freeze().ok());

    const eavp::SelectionPreferences explicit_preference(
        std::vector<std::string>{"z.preferred"}, true, true);
    const eavp::Result<eavp::EncoderSelection> preferred =
        registry.select_video_encoder(
            make_encoder_request_with_preferences(explicit_preference));
    ASSERT_TRUE(preferred.ok());
    EXPECT_EQ("z.preferred", preferred.value().negotiation.provider_id);

    const eavp::SelectionPreferences hardware_and_zero_copy(
        std::vector<std::string>(), true, true);
    const eavp::Result<eavp::EncoderSelection> zero_copy =
        registry.select_video_encoder(make_encoder_request_with_preferences(
            hardware_and_zero_copy));
    ASSERT_TRUE(zero_copy.ok());
    EXPECT_EQ("c.hardware-zero", zero_copy.value().negotiation.provider_id);

    const eavp::SelectionPreferences hardware_only(std::vector<std::string>(),
                                                    true, false);
    const eavp::Result<eavp::EncoderSelection> hardware =
        registry.select_video_encoder(
            make_encoder_request_with_preferences(hardware_only));
    ASSERT_TRUE(hardware.ok());
    EXPECT_EQ("b.hardware-copy", hardware.value().negotiation.provider_id);

    const eavp::Result<eavp::EncoderSelection> lexical =
        registry.select_video_encoder(
            make_encoder_request_with_preferences(no_preferences()));
    ASSERT_TRUE(lexical.ok());
    EXPECT_EQ("a.reference", lexical.value().negotiation.provider_id);
}

TEST(BackendRegistryTest, RequiredUnavailableProviderDoesNotFallBack) {
    eavp::BackendRegistry registry;
    ASSERT_TRUE(registry
                    .register_provider(make_test_provider(
                        "required.gone", false,
                        eavp::ProviderKind::kHardware))
                    .ok());
    ASSERT_TRUE(registry
                    .register_provider(make_test_provider(
                        "healthy", true, eavp::ProviderKind::kSoftware))
                    .ok());
    ASSERT_TRUE(registry.freeze().ok());

    const eavp::Result<eavp::EncoderSelection> result =
        registry.select_video_encoder(make_encoder_request_with_preferences(
            no_preferences(), "required.gone"));
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(eavp::StatusCode::kDeviceLost, result.status().code());
    EXPECT_EQ("test device is unavailable", result.status().message());
}

TEST(BackendRegistryTest, RequiredUnregisteredProcessorReturnsNotFound) {
    eavp::BackendRegistry registry;
    ASSERT_TRUE(
        registry.register_provider(make_test_provider("healthy", true)).ok());
    ASSERT_TRUE(registry.freeze().ok());

    const eavp::Result<eavp::ProcessorSelection> result =
        registry.select_video_processor(
            make_processor_request(no_preferences(), "missing.processor"));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(eavp::StatusCode::kNotFound, result.status().code());
    EXPECT_NE(std::string::npos,
              result.status().message().find("missing.processor"));
}

TEST(BackendRegistryTest, RequiredUnregisteredEncoderReturnsNotFound) {
    eavp::BackendRegistry registry;
    ASSERT_TRUE(
        registry.register_provider(make_test_provider("healthy", true)).ok());
    ASSERT_TRUE(registry.freeze().ok());

    const eavp::Result<eavp::EncoderSelection> result =
        registry.select_video_encoder(make_encoder_request_with_preferences(
            no_preferences(), "missing.encoder"));

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(eavp::StatusCode::kNotFound, result.status().code());
    EXPECT_NE(std::string::npos,
              result.status().message().find("missing.encoder"));
}

TEST(BackendRegistryTest, AggregatesEachCandidatesFirstRejectionReason) {
    eavp::BackendRegistry registry;
    ASSERT_TRUE(
        registry.register_provider(make_test_provider("provider.a", true)).ok());
    ASSERT_TRUE(
        registry.register_provider(make_test_provider("provider.b", true)).ok());
    ASSERT_TRUE(registry.freeze().ok());
    const eavp::VideoEncoderRequest unsupported = make_encoder_request(
        1920, 1080, eavp::MemoryDomain::kCpu);

    const eavp::Result<eavp::EncoderSelection> result =
        registry.select_video_encoder(unsupported);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch, result.status().code());
    EXPECT_NE(std::string::npos, result.status().message().find("provider.a"));
    EXPECT_NE(std::string::npos, result.status().message().find("provider.b"));
    EXPECT_NE(std::string::npos,
              result.status().message().find("memory domain"));
}

TEST(BackendInterfaceTest, IsMoveOnlyAndDestroysThroughBaseUniquePointer) {
    static_assert(!std::is_copy_constructible<TestEncoder>::value,
                  "backend instances must not be copy constructible");
    static_assert(!std::is_copy_assignable<TestEncoder>::value,
                  "backend instances must not be copy assignable");

    bool destroyed = false;
    {
        std::unique_ptr<eavp::VideoEncoder> encoder(
            new TestEncoder(&destroyed));
        EXPECT_EQ(eavp::BackendState::kCreated, encoder->state());
    }
    EXPECT_TRUE(destroyed);

    const std::shared_ptr<eavp::MediaBackendProvider> provider =
        make_test_provider("factory", true);
    eavp::Result<std::unique_ptr<eavp::VideoEncoder> > factory_result =
        provider->create_video_encoder();
    ASSERT_TRUE(factory_result.ok());
    std::unique_ptr<eavp::VideoEncoder> factory_encoder =
        factory_result.take_value();
    EXPECT_EQ(eavp::BackendState::kCreated, factory_encoder->state());
}

TEST(BackendInterfaceTest,
     EncoderRejectsEveryLifecycleAndDataCallFromAnotherThread) {
    TestEncoder encoder;
    ASSERT_TRUE(encoder
                    .configure(
                        make_nv12_format(1920, 1080,
                                         eavp::MemoryDomain::kDmaBuf),
                        make_h264_config(
                            1920, 1080, eavp::CodecProfile::kH264Main))
                    .ok());

    eavp::StatusCode submit_code = eavp::StatusCode::kOk;
    eavp::StatusCode receive_code = eavp::StatusCode::kOk;
    eavp::StatusCode drain_code = eavp::StatusCode::kOk;
    eavp::StatusCode reset_code = eavp::StatusCode::kOk;
    eavp::StatusCode configure_code = eavp::StatusCode::kOk;
    std::thread other([&encoder, &submit_code, &receive_code, &drain_code,
                       &reset_code, &configure_code]() {
        configure_code =
            encoder
                .configure(
                    make_nv12_format(1920, 1080,
                                     eavp::MemoryDomain::kDmaBuf),
                    make_h264_config(
                        1920, 1080, eavp::CodecProfile::kH264Main))
                .code();
        submit_code = encoder
                          .submit(std::shared_ptr<const eavp::VideoFrame>())
                          .code();
        receive_code = encoder.receive().status().code();
        drain_code = encoder.begin_drain().code();
        reset_code = encoder.reset().code();
    });
    other.join();

    EXPECT_EQ(eavp::StatusCode::kInvalidState, configure_code);
    EXPECT_EQ(eavp::StatusCode::kInvalidState, submit_code);
    EXPECT_EQ(eavp::StatusCode::kInvalidState, receive_code);
    EXPECT_EQ(eavp::StatusCode::kInvalidState, drain_code);
    EXPECT_EQ(eavp::StatusCode::kInvalidState, reset_code);
    EXPECT_EQ(eavp::BackendState::kConfigured, encoder.state());
}

TEST(BackendInterfaceTest, ProcessorUsesTheSameThreadAffinityContract) {
    TestProcessor processor;
    ASSERT_TRUE(processor.configure(make_processor_request().config()).ok());

    eavp::StatusCode submit_code = eavp::StatusCode::kOk;
    std::thread other([&processor, &submit_code]() {
        submit_code = processor
                          .submit(std::shared_ptr<const eavp::VideoFrame>())
                          .code();
    });
    other.join();

    EXPECT_EQ(eavp::StatusCode::kInvalidState, submit_code);
    EXPECT_EQ(eavp::BackendState::kConfigured, processor.state());
}

}  // namespace
