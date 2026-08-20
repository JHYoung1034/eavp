#ifndef EAVP_TESTS_SUPPORT_BACKEND_CONTRACT_HPP_
#define EAVP_TESTS_SUPPORT_BACKEND_CONTRACT_HPP_

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "eavp/media/backend.hpp"
#include "eavp/media/reference_backend.hpp"

namespace backend_contract_detail {

class CpuStorage : public eavp::BufferStorage {
public:
    explicit CpuStorage(std::size_t size)
        : bytes_(size), provider_id_("contract.cpu") {}

    eavp::MemoryDomain memory_domain() const { return eavp::MemoryDomain::kCpu; }
    std::size_t capacity() const { return bytes_.size(); }
    const std::string& provider_id() const { return provider_id_; }

    eavp::Status map(eavp::MapMode, std::uint8_t** data, std::size_t* size) {
        *data = bytes_.data();
        *size = bytes_.size();
        return eavp::Status::ok_status();
    }

    eavp::Status unmap() { return eavp::Status::ok_status(); }

    eavp::Result<eavp::NativeBufferHandle> export_dmabuf() const {
        return eavp::Result<eavp::NativeBufferHandle>(
            eavp::Status(eavp::StatusCode::kUnsupported));
    }

private:
    std::vector<std::uint8_t> bytes_;
    std::string provider_id_;
};

struct FrameStorageProbe {
    FrameStorageProbe(
        const std::shared_ptr<const eavp::VideoFrame>& frame_value,
        const std::weak_ptr<eavp::BufferStorage>& storage_value)
        : frame(frame_value), storage(storage_value) {}

    std::shared_ptr<const eavp::VideoFrame> frame;
    std::weak_ptr<eavp::BufferStorage> storage;
};

inline eavp::VideoFormat make_reference_format() {
    const std::vector<eavp::PlaneLayout> planes{
        eavp::PlaneLayout(0U, 12U, 6U)};
    return eavp::VideoFormat::create(eavp::PixelFormat::kRgb24, 2, 2,
                                     eavp::MemoryDomain::kCpu, planes)
        .take_value();
}

inline eavp::VideoProcessorConfig make_reference_processor_config() {
    const eavp::VideoFormat format = make_reference_format();
    return eavp::VideoProcessorConfig::create(format, format, 0, 0, 2, 2, 0)
        .take_value();
}

inline eavp::VideoProcessorConfig make_non_identity_reference_processor_config() {
    const eavp::VideoFormat input = make_reference_format();
    const std::vector<eavp::PlaneLayout> planes{
        eavp::PlaneLayout(0U, 12U, 6U)};
    const eavp::VideoFormat output =
        eavp::VideoFormat::create(
            eavp::PixelFormat::kRgb24, 2, 2, eavp::MemoryDomain::kCpu,
            planes, eavp::ColorRange::kFull)
            .take_value();
    return eavp::VideoProcessorConfig::create(input, output, 0, 0, 2, 2, 0)
        .take_value();
}

inline eavp::VideoEncoderConfig make_reference_encoder_config(
    eavp::CodecId codec = eavp::CodecId::kReference) {
    return eavp::VideoEncoderConfig::create(
               codec, 2, 2, 30, 1, eavp::TimeBase::create(1, 30).take_value(),
               1, 1, 1, 0, eavp::RateControlMode::kConstantQuality,
               eavp::CodecProfile::kUnknown, 0, true)
        .take_value();
}

inline FrameStorageProbe make_frame_storage_probe(std::int64_t pts) {
    const eavp::VideoFormat format = make_reference_format();
    std::shared_ptr<eavp::BufferStorage> storage(new CpuStorage(12U));
    eavp::Buffer buffer =
        eavp::Buffer::create(storage, format.planes()).take_value();
    eavp::Result<eavp::MappedRegion> mapped =
        buffer.map_plane(0U, eavp::MapMode::kReadWrite);
    if (!mapped.ok()) {
        ADD_FAILURE() << "契约测试帧必须可映射";
        return FrameStorageProbe(
            std::shared_ptr<const eavp::VideoFrame>(),
            std::weak_ptr<eavp::BufferStorage>());
    }
    for (std::size_t index = 0U; index < mapped.value().size(); ++index) {
        mapped.value().mutable_data()[index] = static_cast<std::uint8_t>(index);
    }
    eavp::Result<eavp::VideoFrame> frame = eavp::VideoFrame::create(
        buffer, format, pts, eavp::TimeBase::create(1, 30).take_value());
    if (!frame.ok()) {
        ADD_FAILURE() << "契约测试帧必须有效";
        return FrameStorageProbe(
            std::shared_ptr<const eavp::VideoFrame>(),
            std::weak_ptr<eavp::BufferStorage>());
    }
    return FrameStorageProbe(
        std::shared_ptr<const eavp::VideoFrame>(
            new eavp::VideoFrame(frame.take_value())),
        storage);
}

inline std::shared_ptr<const eavp::VideoFrame> make_frame(std::int64_t pts) {
    return make_frame_storage_probe(pts).frame;
}

class ReferenceFixture {
public:
    eavp::VideoProcessorConfig processor_config() const {
        return make_reference_processor_config();
    }

    eavp::VideoProcessorConfig unsupported_processor_config() const {
        return make_non_identity_reference_processor_config();
    }

    eavp::VideoFormat encoder_input_format() const {
        return make_reference_format();
    }

    eavp::VideoEncoderConfig encoder_config() const {
        return make_reference_encoder_config();
    }

    eavp::VideoEncoderConfig unsupported_encoder_config() const {
        return make_reference_encoder_config(eavp::CodecId::kH264);
    }

    std::shared_ptr<const eavp::VideoFrame> frame(std::int64_t pts) const {
        return make_frame(pts);
    }

    FrameStorageProbe frame_storage_probe(std::int64_t pts) const {
        return make_frame_storage_probe(pts);
    }
};

inline std::unique_ptr<eavp::VideoProcessor> create_processor(
    const std::shared_ptr<eavp::MediaBackendProvider>& provider) {
    eavp::Result<std::unique_ptr<eavp::VideoProcessor> > result =
        provider->create_video_processor();
    if (!result.ok()) {
        ADD_FAILURE() << "Capability 声明 Processor 时 factory 必须成功";
        return std::unique_ptr<eavp::VideoProcessor>();
    }
    return result.take_value();
}

inline std::unique_ptr<eavp::VideoEncoder> create_encoder(
    const std::shared_ptr<eavp::MediaBackendProvider>& provider) {
    eavp::Result<std::unique_ptr<eavp::VideoEncoder> > result =
        provider->create_video_encoder();
    if (!result.ok()) {
        ADD_FAILURE() << "Capability 声明 Encoder 时 factory 必须成功";
        return std::unique_ptr<eavp::VideoEncoder>();
    }
    return result.take_value();
}

template <typename Fixture>
inline void verify_capability_and_unsupported_config(
    const std::shared_ptr<eavp::MediaBackendProvider>& provider,
    const Fixture& fixture) {
    eavp::Result<eavp::ProviderCapability> probe = provider->probe();
    ASSERT_TRUE(probe.ok());
    ASSERT_TRUE(probe.value().available());
    ASSERT_FALSE(probe.value().processor_capabilities().empty());
    ASSERT_FALSE(probe.value().encoder_capabilities().empty());

    const eavp::SelectionConstraints automatic("");
    const eavp::SelectionPreferences no_preferences(
        std::vector<std::string>(), false, false);
    const eavp::VideoProcessorRequest processor_request(
        fixture.processor_config(),
        std::vector<eavp::VideoProcessingOperation>(), 1U, 1U, true,
        automatic, no_preferences);
    const eavp::VideoEncoderRequest encoder_request(
        fixture.encoder_input_format(), fixture.encoder_config(), 1U, true,
        automatic, no_preferences);
    EXPECT_TRUE(probe.value().supports(processor_request));
    EXPECT_TRUE(probe.value().supports(encoder_request));
    eavp::Result<eavp::VideoProcessorNegotiation> processor_negotiation =
        probe.value().negotiate(processor_request);
    eavp::Result<eavp::VideoEncoderNegotiation> encoder_negotiation =
        probe.value().negotiate(encoder_request);
    ASSERT_TRUE(processor_negotiation.ok());
    ASSERT_TRUE(encoder_negotiation.ok());
    EXPECT_TRUE(processor_negotiation.value().zero_copy);
    EXPECT_TRUE(encoder_negotiation.value().zero_copy);
    EXPECT_FALSE(processor_negotiation.value().requires_explicit_conversion);
    EXPECT_FALSE(encoder_negotiation.value().requires_explicit_conversion);

    const eavp::VideoProcessorRequest unsupported_processor_request(
        fixture.unsupported_processor_config(),
        std::vector<eavp::VideoProcessingOperation>(), 1U, 1U, false,
        automatic, no_preferences);
    const eavp::VideoEncoderRequest unsupported_encoder_request(
        fixture.encoder_input_format(), fixture.unsupported_encoder_config(),
        1U, false, automatic, no_preferences);
    EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch,
              probe.value().match(unsupported_processor_request).code())
        << "Capability 不得声明实例 configure 会拒绝的 Processor 配置";
    EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch,
              probe.value().match(unsupported_encoder_request).code())
        << "Capability 不得声明实例 configure 会拒绝的 Encoder 配置";

    std::unique_ptr<eavp::VideoProcessor> processor = create_processor(provider);
    ASSERT_TRUE(processor.get() != NULL);
    EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch,
              processor->configure(fixture.unsupported_processor_config()).code());
    EXPECT_EQ(eavp::BackendState::kCreated, processor->state());
    EXPECT_TRUE(processor->configure(fixture.processor_config()).ok());

    std::unique_ptr<eavp::VideoEncoder> encoder = create_encoder(provider);
    ASSERT_TRUE(encoder.get() != NULL);
    EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch,
              encoder
                  ->configure(fixture.encoder_input_format(),
                              fixture.unsupported_encoder_config())
                  .code());
    EXPECT_EQ(eavp::BackendState::kCreated, encoder->state());
    EXPECT_TRUE(encoder
                    ->configure(fixture.encoder_input_format(),
                                fixture.encoder_config())
                    .ok());
}

template <typename Fixture>
inline void verify_processor_backpressure_lifetime_drain_and_reset(
    const std::shared_ptr<eavp::MediaBackendProvider>& provider,
    const Fixture& fixture) {
    std::unique_ptr<eavp::VideoProcessor> processor = create_processor(provider);
    ASSERT_TRUE(processor.get() != NULL);
    ASSERT_TRUE(processor->configure(fixture.processor_config()).ok());

    std::vector<std::int64_t> submitted_pts;
    std::vector<std::weak_ptr<eavp::BufferStorage> > pending_storage;
    for (std::int64_t pts = 0; pts < 128; ++pts) {
        FrameStorageProbe probe = fixture.frame_storage_probe(pts);
        const eavp::Status status = processor->submit(probe.frame);
        if (status.code() == eavp::StatusCode::kWouldBlock) {
            break;
        }
        ASSERT_TRUE(status.ok());
        submitted_pts.push_back(pts);
        pending_storage.push_back(probe.storage);
        probe.frame.reset();
    }
    ASSERT_FALSE(submitted_pts.empty());
    ASSERT_LT(submitted_pts.size(), 128U);
    for (std::size_t index = 0U; index < pending_storage.size(); ++index) {
        EXPECT_FALSE(pending_storage[index].expired());
    }

    for (std::size_t index = 0U; index < submitted_pts.size(); ++index) {
        {
            eavp::Result<std::shared_ptr<const eavp::VideoFrame> > output =
                processor->receive();
            ASSERT_TRUE(output.ok());
            EXPECT_EQ(submitted_pts[index], output.value()->pts());
            EXPECT_FALSE(pending_storage[index].expired());
        }
        EXPECT_TRUE(pending_storage[index].expired());
    }
    ASSERT_TRUE(processor->begin_drain().ok());
    EXPECT_EQ(eavp::StatusCode::kEndOfStream,
              processor->receive().status().code());
    EXPECT_EQ(eavp::StatusCode::kEndOfStream,
              processor->receive().status().code());
    EXPECT_EQ(eavp::BackendState::kStopped, processor->state());

    ASSERT_TRUE(processor->reset().ok());
    EXPECT_EQ(eavp::BackendState::kCreated, processor->state());
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              processor->receive().status().code());

    ASSERT_TRUE(processor->configure(fixture.processor_config()).ok());
    FrameStorageProbe reset_probe = fixture.frame_storage_probe(150);
    ASSERT_TRUE(processor->submit(reset_probe.frame).ok());
    reset_probe.frame.reset();
    EXPECT_FALSE(reset_probe.storage.expired());
    ASSERT_TRUE(processor->reset().ok());
    EXPECT_TRUE(reset_probe.storage.expired());
}

template <typename Fixture>
inline void verify_encoder_backpressure_lifetime_drain_and_reset(
    const std::shared_ptr<eavp::MediaBackendProvider>& provider,
    const Fixture& fixture) {
    std::unique_ptr<eavp::VideoEncoder> encoder = create_encoder(provider);
    ASSERT_TRUE(encoder.get() != NULL);
    ASSERT_TRUE(encoder
                    ->configure(fixture.encoder_input_format(),
                                fixture.encoder_config())
                    .ok());

    std::vector<std::int64_t> submitted_pts;
    std::vector<std::weak_ptr<eavp::BufferStorage> > pending_storage;
    for (std::int64_t pts = 0; pts < 128; ++pts) {
        FrameStorageProbe probe = fixture.frame_storage_probe(pts);
        const eavp::Status status = encoder->submit(probe.frame);
        if (status.code() == eavp::StatusCode::kWouldBlock) {
            break;
        }
        ASSERT_TRUE(status.ok());
        submitted_pts.push_back(pts);
        pending_storage.push_back(probe.storage);
        probe.frame.reset();
    }
    ASSERT_FALSE(submitted_pts.empty());
    ASSERT_LT(submitted_pts.size(), 128U);
    for (std::size_t index = 0U; index < pending_storage.size(); ++index) {
        EXPECT_FALSE(pending_storage[index].expired());
    }

    ASSERT_TRUE(encoder->begin_drain().ok());
    for (std::size_t index = 0U; index < submitted_pts.size(); ++index) {
        eavp::Result<std::shared_ptr<const eavp::MediaPacket> > output =
            encoder->receive();
        ASSERT_TRUE(output.ok());
        EXPECT_EQ(submitted_pts[index], output.value()->pts());
        EXPECT_TRUE(pending_storage[index].expired());
    }
    EXPECT_EQ(eavp::StatusCode::kEndOfStream,
              encoder->receive().status().code());
    EXPECT_EQ(eavp::StatusCode::kEndOfStream,
              encoder->receive().status().code());
    EXPECT_EQ(eavp::BackendState::kStopped, encoder->state());

    ASSERT_TRUE(encoder->reset().ok());
    EXPECT_EQ(eavp::BackendState::kCreated, encoder->state());
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              encoder->receive().status().code());

    ASSERT_TRUE(encoder
                    ->configure(fixture.encoder_input_format(),
                                fixture.encoder_config())
                    .ok());
    FrameStorageProbe reset_probe = fixture.frame_storage_probe(160);
    ASSERT_TRUE(encoder->submit(reset_probe.frame).ok());
    reset_probe.frame.reset();
    EXPECT_FALSE(reset_probe.storage.expired());
    ASSERT_TRUE(encoder->reset().ok());
    EXPECT_TRUE(reset_probe.storage.expired());
}

template <typename Fixture>
inline void verify_thread_affinity_and_destruction(
    const std::shared_ptr<eavp::MediaBackendProvider>& provider,
    const Fixture& fixture) {
    std::unique_ptr<eavp::VideoProcessor> processor = create_processor(provider);
    ASSERT_TRUE(processor.get() != NULL);
    ASSERT_TRUE(processor->configure(fixture.processor_config()).ok());
    eavp::StatusCode processor_code = eavp::StatusCode::kOk;
    eavp::StatusCode processor_configure_code = eavp::StatusCode::kOk;
    eavp::StatusCode processor_receive_code = eavp::StatusCode::kOk;
    eavp::StatusCode processor_drain_code = eavp::StatusCode::kOk;
    eavp::StatusCode processor_reset_code = eavp::StatusCode::kOk;
    std::thread processor_caller([
        &processor, &processor_code, &processor_configure_code,
        &processor_receive_code, &processor_drain_code,
        &processor_reset_code, &fixture]() {
        processor_configure_code =
            processor->configure(fixture.processor_config()).code();
        processor_code = processor->submit(fixture.frame(200)).code();
        processor_receive_code = processor->receive().status().code();
        processor_drain_code = processor->begin_drain().code();
        processor_reset_code = processor->reset().code();
    });
    processor_caller.join();
    EXPECT_EQ(eavp::StatusCode::kInvalidState, processor_configure_code);
    EXPECT_EQ(eavp::StatusCode::kInvalidState, processor_code);
    EXPECT_EQ(eavp::StatusCode::kInvalidState, processor_receive_code);
    EXPECT_EQ(eavp::StatusCode::kInvalidState, processor_drain_code);
    EXPECT_EQ(eavp::StatusCode::kInvalidState, processor_reset_code);
    EXPECT_EQ(eavp::BackendState::kConfigured, processor->state());

    FrameStorageProbe processor_destruction_probe =
        fixture.frame_storage_probe(201);
    ASSERT_TRUE(processor->submit(processor_destruction_probe.frame).ok());
    processor_destruction_probe.frame.reset();
    EXPECT_FALSE(processor_destruction_probe.storage.expired());
    eavp::VideoProcessor* raw_processor = processor.release();
    std::thread processor_destroyer([raw_processor]() { delete raw_processor; });
    processor_destroyer.join();
    EXPECT_TRUE(processor_destruction_probe.storage.expired());

    std::unique_ptr<eavp::VideoEncoder> encoder = create_encoder(provider);
    ASSERT_TRUE(encoder.get() != NULL);
    ASSERT_TRUE(encoder
                    ->configure(fixture.encoder_input_format(),
                                fixture.encoder_config())
                    .ok());
    eavp::StatusCode encoder_code = eavp::StatusCode::kOk;
    eavp::StatusCode encoder_configure_code = eavp::StatusCode::kOk;
    eavp::StatusCode encoder_receive_code = eavp::StatusCode::kOk;
    eavp::StatusCode encoder_drain_code = eavp::StatusCode::kOk;
    eavp::StatusCode encoder_reset_code = eavp::StatusCode::kOk;
    std::thread encoder_caller([
        &encoder, &encoder_code, &encoder_configure_code,
        &encoder_receive_code, &encoder_drain_code, &encoder_reset_code,
        &fixture]() {
        encoder_configure_code =
            encoder
                ->configure(fixture.encoder_input_format(),
                            fixture.encoder_config())
                .code();
        encoder_code = encoder->submit(fixture.frame(300)).code();
        encoder_receive_code = encoder->receive().status().code();
        encoder_drain_code = encoder->begin_drain().code();
        encoder_reset_code = encoder->reset().code();
    });
    encoder_caller.join();
    EXPECT_EQ(eavp::StatusCode::kInvalidState, encoder_configure_code);
    EXPECT_EQ(eavp::StatusCode::kInvalidState, encoder_code);
    EXPECT_EQ(eavp::StatusCode::kInvalidState, encoder_receive_code);
    EXPECT_EQ(eavp::StatusCode::kInvalidState, encoder_drain_code);
    EXPECT_EQ(eavp::StatusCode::kInvalidState, encoder_reset_code);
    EXPECT_EQ(eavp::BackendState::kConfigured, encoder->state());

    FrameStorageProbe encoder_destruction_probe =
        fixture.frame_storage_probe(301);
    ASSERT_TRUE(encoder->submit(encoder_destruction_probe.frame).ok());
    encoder_destruction_probe.frame.reset();
    EXPECT_FALSE(encoder_destruction_probe.storage.expired());
    eavp::VideoEncoder* raw_encoder = encoder.release();
    std::thread encoder_destroyer([raw_encoder]() { delete raw_encoder; });
    encoder_destroyer.join();
    EXPECT_TRUE(encoder_destruction_probe.storage.expired());
}

}  // namespace backend_contract_detail

inline std::shared_ptr<const eavp::VideoFrame> make_reference_frame(
    std::int64_t pts) {
    return backend_contract_detail::make_frame(pts);
}

inline std::unique_ptr<eavp::VideoEncoder>
create_configured_reference_encoder(
    const eavp::ReferenceBackendOptions& options) {
    const std::shared_ptr<eavp::MediaBackendProvider> provider =
        eavp::create_reference_backend(options);
    if (!provider) {
        ADD_FAILURE() << "Reference Provider 创建失败";
        return std::unique_ptr<eavp::VideoEncoder>();
    }
    std::unique_ptr<eavp::VideoEncoder> encoder =
        backend_contract_detail::create_encoder(provider);
    if (!encoder) {
        return encoder;
    }
    const eavp::Status status = encoder->configure(
        backend_contract_detail::make_reference_format(),
        backend_contract_detail::make_reference_encoder_config());
    if (!status.ok()) {
        ADD_FAILURE() << "Reference Encoder 配置失败";
        return std::unique_ptr<eavp::VideoEncoder>();
    }
    return encoder;
}

template <typename Fixture>
inline void run_backend_contract(
    const std::shared_ptr<eavp::MediaBackendProvider>& provider,
    const Fixture& fixture) {
    ASSERT_TRUE(provider.get() != NULL);
    backend_contract_detail::verify_capability_and_unsupported_config(
        provider, fixture);
    backend_contract_detail::verify_processor_backpressure_lifetime_drain_and_reset(
        provider, fixture);
    backend_contract_detail::verify_encoder_backpressure_lifetime_drain_and_reset(
        provider, fixture);
    backend_contract_detail::verify_thread_affinity_and_destruction(provider,
                                                                    fixture);
}

inline void run_backend_contract(
    const std::shared_ptr<eavp::MediaBackendProvider>& provider) {
    const backend_contract_detail::ReferenceFixture fixture;
    run_backend_contract(provider, fixture);
}

#endif  // EAVP_TESTS_SUPPORT_BACKEND_CONTRACT_HPP_
