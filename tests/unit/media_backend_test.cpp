#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "eavp/media/capability.hpp"

namespace {

eavp::VideoFormat make_nv12_format(int width, int height,
                                   eavp::MemoryDomain memory_domain) {
    const std::vector<eavp::PlaneLayout> planes{
        eavp::PlaneLayout(0, static_cast<std::size_t>(width * height),
                          static_cast<std::size_t>(width)),
        eavp::PlaneLayout(static_cast<std::size_t>(width * height),
                          static_cast<std::size_t>(width * height / 2),
                          static_cast<std::size_t>(width))};
    eavp::Result<eavp::VideoFormat> result = eavp::VideoFormat::create(
        eavp::PixelFormat::kNv12, width, height, memory_domain, planes);
    EXPECT_TRUE(result.ok());
    return result.take_value();
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

eavp::VideoEncoderCapability make_h264_capability_for_test(
    bool zero_copy = true) {
    return eavp::VideoEncoderCapability(
        eavp::DimensionRange(16, 1920, 2, 16),
        eavp::DimensionRange(16, 1088, 2, 16), eavp::CodecId::kH264,
        std::vector<eavp::CodecProfile>{eavp::CodecProfile::kH264Main},
        std::vector<int>{40},
        std::vector<eavp::FormatMemoryDomain>{eavp::FormatMemoryDomain(
            eavp::PixelFormat::kNv12, eavp::MemoryDomain::kDmaBuf)},
        std::vector<eavp::RateControlMode>{eavp::RateControlMode::kCbr},
        zero_copy);
}

eavp::VideoEncoderRequest make_encoder_request(
    int width, int height, eavp::MemoryDomain memory_domain,
    bool require_zero_copy = false,
    eavp::CodecProfile profile = eavp::CodecProfile::kH264Main,
    int level_idc = 40) {
    const int valid_input_width = width % 2 == 0 ? width : width + 1;
    return eavp::VideoEncoderRequest(
        make_nv12_format(valid_input_width, height, memory_domain),
        make_h264_config(width, height, profile, level_idc), require_zero_copy,
        no_preferences());
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

    EXPECT_FALSE(capability.supports(make_encoder_request(
        1919, 1080, eavp::MemoryDomain::kDmaBuf)));
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
        std::vector<eavp::FormatMemoryDomain>{eavp::FormatMemoryDomain(
            eavp::PixelFormat::kNv12, eavp::MemoryDomain::kDmaBuf)},
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
        vbr.take_value(), false, no_preferences())));
}

TEST(CapabilityTest, RequiredZeroCopyRejectsButPreferenceDoesNot) {
    const eavp::VideoEncoderCapability software_capability =
        make_h264_capability_for_test(false);

    EXPECT_FALSE(software_capability.supports(make_encoder_request(
        1920, 1080, eavp::MemoryDomain::kDmaBuf, true)));

    eavp::VideoEncoderRequest preferred_request(
        make_nv12_format(1920, 1080, eavp::MemoryDomain::kDmaBuf),
        make_h264_config(1920, 1080, eavp::CodecProfile::kH264Main), false,
        eavp::SelectionPreferences(std::vector<std::string>(), true, true));
    EXPECT_TRUE(software_capability.supports(preferred_request));
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
        false, no_preferences());
    const eavp::VideoProcessorCapability capability(
        eavp::DimensionRange(16, 1920, 2, 16),
        eavp::DimensionRange(16, 1088, 2, 16),
        eavp::DimensionRange(16, 1920, 2, 16),
        eavp::DimensionRange(16, 1088, 2, 16),
        std::vector<eavp::FormatMemoryDomain>{eavp::FormatMemoryDomain(
            eavp::PixelFormat::kNv12, eavp::MemoryDomain::kDmaBuf)},
        std::vector<eavp::FormatMemoryDomain>{eavp::FormatMemoryDomain(
            eavp::PixelFormat::kNv12, eavp::MemoryDomain::kDmaBuf)},
        std::vector<eavp::VideoProcessingOperation>(), true);

    const eavp::Status status = capability.match(request);
    EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch, status.code());
    EXPECT_NE(std::string::npos, status.message().find("scaling"));
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
        true, no_preferences());
    const eavp::VideoProcessorCapability processor_capability(
        eavp::DimensionRange(16, 1920, 2, 16),
        eavp::DimensionRange(16, 1088, 2, 16),
        eavp::DimensionRange(16, 1920, 2, 16),
        eavp::DimensionRange(16, 1088, 2, 16),
        std::vector<eavp::FormatMemoryDomain>{eavp::FormatMemoryDomain(
            eavp::PixelFormat::kNv12, eavp::MemoryDomain::kDmaBuf)},
        std::vector<eavp::FormatMemoryDomain>{eavp::FormatMemoryDomain(
            eavp::PixelFormat::kNv12, eavp::MemoryDomain::kDmaBuf)},
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
        make_h264_config(1920, 1080, eavp::CodecProfile::kH264Main), false,
        preferences);

    EXPECT_TRUE(software.supports(request));
    EXPECT_TRUE(hardware.supports(request));
    EXPECT_LT(software.preference_score(preferences, false),
              hardware.preference_score(preferences, true));

    const eavp::SelectionPreferences no_provider_rank(
        std::vector<std::string>(), true, true);
    EXPECT_LT(hardware.preference_score(no_provider_rank, true),
              software.preference_score(no_provider_rank, false));

    const eavp::ProviderCapability provider_a = make_provider(
        "provider.a", eavp::ProviderKind::kSoftware, false);
    const eavp::ProviderCapability provider_b = make_provider(
        "provider.b", eavp::ProviderKind::kSoftware, false);
    EXPECT_LT(provider_a.preference_score(no_provider_rank, false),
              provider_b.preference_score(no_provider_rank, false));
}

}  // namespace
