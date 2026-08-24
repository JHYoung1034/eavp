#include <memory>
#include <vector>

#include "eavp/media/backend_registry.hpp"
#include "eavp/media/reference_backend.hpp"
#include "eavp/management/health.hpp"
#include "eavp/management/metrics.hpp"
#include "eavp/platform/linux/alsa_capture.hpp"
#include "eavp/platform/simulated_platform.hpp"

int main() {
    const std::vector<eavp::PlaneLayout> planes{
        eavp::PlaneLayout(0U, 12U, 6U)};
    eavp::Result<eavp::Buffer> buffer_result = eavp::Buffer::allocate(12U);
    if (!buffer_result.ok()) {
        return 1;
    }
    eavp::Buffer buffer = buffer_result.take_value();
    eavp::Result<eavp::MappedRegion> mapped =
        buffer.map_plane(0U, eavp::MapMode::kReadWrite);
    if (!mapped.ok() || mapped.value().size() != 12U) {
        return 1;
    }

    eavp::Result<eavp::VideoFormat> format_result = eavp::VideoFormat::create(
        eavp::PixelFormat::kRgb24, 2, 2, eavp::MemoryDomain::kCpu, planes);
    if (!format_result.ok()) {
        return 1;
    }
    const eavp::VideoFormat format = format_result.take_value();
    eavp::Result<eavp::VideoProcessorConfig> config_result =
        eavp::VideoProcessorConfig::create(format, format, 0, 0, 2, 2, 0);
    if (!config_result.ok()) {
        return 1;
    }

    eavp::Result<eavp::AudioFormat> audio_format_result =
        eavp::AudioFormat::create(
            eavp::SampleFormat::kSigned16LittleEndian, 48000,
            eavp::AudioChannelLayout::kStereo,
            eavp::AudioSampleLayout::kInterleaved,
            eavp::MemoryDomain::kCpu);
    if (!audio_format_result.ok()) {
        return 1;
    }
    eavp::Result<eavp::AlsaCaptureConfig> alsa_config_result =
        eavp::AlsaCaptureConfig::create(
            "hw:Loopback,1,0", audio_format_result.value(), 480, 480, 4);
    if (!alsa_config_result.ok()) {
        return 1;
    }
    eavp::MetricRegistry metrics;
    eavp::HealthManager health;
    eavp::Result<std::unique_ptr<eavp::AlsaSourceNode> > alsa_source_result =
        eavp::AlsaSourceNode::create(
            "consumer-alsa", alsa_config_result.value(), &metrics, &health);
    if (!alsa_source_result.ok()) {
        return 1;
    }

    eavp::BackendRegistry registry;
    std::shared_ptr<eavp::MediaBackendProvider> provider =
        eavp::create_reference_backend(eavp::ReferenceBackendOptions());
    if (!provider || !registry.register_provider(provider).ok() ||
        !registry.freeze().ok()) {
        return 1;
    }
    const eavp::VideoProcessorRequest request(
        config_result.take_value(),
        std::vector<eavp::VideoProcessingOperation>(), 1U, 1U, true,
        eavp::SelectionConstraints("reference"),
        eavp::SelectionPreferences(std::vector<std::string>(), false, false));
    eavp::Result<eavp::ProcessorSelection> selection =
        registry.select_video_processor(request);
    if (!selection.ok() || selection.value().provider->probe().status().ok() == false) {
        return 1;
    }

    eavp::SimulatedPlatform platform;
    return platform.initialize().ok() ? 0 : 1;
}
