#ifndef EAVP_MEDIA_BACKEND_REGISTRY_HPP_
#define EAVP_MEDIA_BACKEND_REGISTRY_HPP_

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "eavp/media/backend.hpp"

namespace eavp {

struct ProcessorSelection {
    ProcessorSelection(
        const std::shared_ptr<const MediaBackendProvider>& provider_value,
        VideoProcessorNegotiation&& negotiation_value)
        : provider(provider_value),
          negotiation(std::move(negotiation_value)) {}

    std::shared_ptr<const MediaBackendProvider> provider;
    VideoProcessorNegotiation negotiation;
};

struct EncoderSelection {
    EncoderSelection(
        const std::shared_ptr<const MediaBackendProvider>& provider_value,
        VideoEncoderNegotiation&& negotiation_value)
        : provider(provider_value),
          negotiation(std::move(negotiation_value)) {}

    std::shared_ptr<const MediaBackendProvider> provider;
    VideoEncoderNegotiation negotiation;
};

class BackendRegistry {
public:
    BackendRegistry() : frozen_(false) {}

    Status register_provider(
        const std::shared_ptr<MediaBackendProvider>& provider);
    Status freeze();

    Result<ProcessorSelection> select_video_processor(
        const VideoProcessorRequest& request) const;
    Result<EncoderSelection> select_video_encoder(
        const VideoEncoderRequest& request) const;

private:
    BackendRegistry(const BackendRegistry&) = delete;
    BackendRegistry& operator=(const BackendRegistry&) = delete;

    typedef std::map<std::string,
                     std::shared_ptr<const MediaBackendProvider> >
        ProviderMap;

    ProviderMap providers_;
    bool frozen_;
};

}  // namespace eavp

#endif  // EAVP_MEDIA_BACKEND_REGISTRY_HPP_
