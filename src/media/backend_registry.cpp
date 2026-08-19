#include "eavp/media/backend_registry.hpp"

#include <algorithm>
#include <new>
#include <utility>
#include <vector>

namespace eavp {

namespace {

Status capability_mismatch(const std::string& message) {
    return Status(StatusCode::kCapabilityMismatch, message);
}

Status allocation_failure() {
    return Status(StatusCode::kResourceExhausted);
}

std::string rejection_message(const std::string& provider_id,
                              const Status& status) {
    return provider_id + ": " +
           (status.message().empty() ? "provider rejected request"
                                     : status.message());
}

void append_rejection(std::string* aggregate, const std::string& provider_id,
                      const Status& status) {
    if (!aggregate->empty()) {
        aggregate->append("; ");
    }
    aggregate->append(rejection_message(provider_id, status));
}

struct ProcessorCandidate {
    ProcessorCandidate(
        const std::shared_ptr<const MediaBackendProvider>& provider_value,
        ProviderCapability&& capability_value,
        ProviderPreferenceScore&& score_value)
        : provider(provider_value),
          capability(std::move(capability_value)),
          score(std::move(score_value)) {}

    std::shared_ptr<const MediaBackendProvider> provider;
    ProviderCapability capability;
    ProviderPreferenceScore score;
};

struct EncoderCandidate {
    EncoderCandidate(
        const std::shared_ptr<const MediaBackendProvider>& provider_value,
        ProviderCapability&& capability_value,
        ProviderPreferenceScore&& score_value)
        : provider(provider_value),
          capability(std::move(capability_value)),
          score(std::move(score_value)) {}

    std::shared_ptr<const MediaBackendProvider> provider;
    ProviderCapability capability;
    ProviderPreferenceScore score;
};

bool processor_candidate_less(const ProcessorCandidate& left,
                              const ProcessorCandidate& right) {
    return left.score < right.score;
}

bool encoder_candidate_less(const EncoderCandidate& left,
                            const EncoderCandidate& right) {
    return left.score < right.score;
}

}  // namespace

Status BackendRegistry::register_provider(
    const std::shared_ptr<MediaBackendProvider>& provider) {
    if (frozen_) {
        return Status(StatusCode::kInvalidState,
                      "backend registry is frozen");
    }
    if (!provider) {
        return Status(StatusCode::kInvalidArgument,
                      "backend provider must not be null");
    }

    try {
        Result<ProviderCapability> probed = provider->probe();
        if (!probed.ok()) {
            return probed.status();
        }
        const std::string provider_id = probed.value().provider_id();
        if (provider_id.empty()) {
            return Status(StatusCode::kInvalidArgument,
                          "backend provider id must not be empty");
        }
        if (providers_.find(provider_id) != providers_.end()) {
            return Status(StatusCode::kAlreadyExists,
                          "backend provider id is already registered");
        }
        providers_.insert(std::make_pair(
            provider_id,
            std::shared_ptr<const MediaBackendProvider>(provider)));
    } catch (const std::bad_alloc&) {
        return allocation_failure();
    }
    return Status::ok_status();
}

Status BackendRegistry::freeze() {
    frozen_ = true;
    return Status::ok_status();
}

Result<ProcessorSelection> BackendRegistry::select_video_processor(
    const VideoProcessorRequest& request) const {
    try {
        const std::string& required_provider_id =
            request.constraints().required_provider_id();
        if (!required_provider_id.empty()) {
            const ProviderMap::const_iterator required =
                providers_.find(required_provider_id);
            if (required == providers_.end()) {
                return Result<ProcessorSelection>(Status(
                    StatusCode::kNotFound,
                    "required backend provider is not registered: " +
                    required_provider_id));
            }
            Result<ProviderCapability> probed = required->second->probe();
            if (!probed.ok()) {
                return Result<ProcessorSelection>(probed.status());
            }
            const Status matched = probed.value().match(request);
            if (!matched.ok()) {
                return Result<ProcessorSelection>(matched);
            }
            Result<VideoProcessorNegotiation> negotiation =
                probed.value().negotiate(request);
            if (!negotiation.ok()) {
                return Result<ProcessorSelection>(negotiation.status());
            }
            return Result<ProcessorSelection>(ProcessorSelection(
                required->second, negotiation.take_value()));
        }

        std::vector<ProcessorCandidate> candidates;
        std::string rejections;
        for (ProviderMap::const_iterator provider = providers_.begin();
             provider != providers_.end(); ++provider) {
            Result<ProviderCapability> probed = provider->second->probe();
            if (!probed.ok()) {
                append_rejection(&rejections, provider->first,
                                 probed.status());
                continue;
            }
            const Status matched = probed.value().match(request);
            if (!matched.ok()) {
                append_rejection(&rejections, provider->first, matched);
                continue;
            }
            Result<ProviderPreferenceScore> score =
                probed.value().preference_score(request);
            if (!score.ok()) {
                return Result<ProcessorSelection>(score.status());
            }
            candidates.push_back(ProcessorCandidate(
                provider->second, probed.take_value(), score.take_value()));
        }
        if (candidates.empty()) {
            if (rejections.empty()) {
                rejections = "no backend providers are registered";
            }
            return Result<ProcessorSelection>(capability_mismatch(
                "no video processor provider matched: " + rejections));
        }

        std::sort(candidates.begin(), candidates.end(),
                  processor_candidate_less);
        Result<VideoProcessorNegotiation> negotiation =
            candidates[0].capability.negotiate(request);
        if (!negotiation.ok()) {
            return Result<ProcessorSelection>(negotiation.status());
        }
        return Result<ProcessorSelection>(ProcessorSelection(
            candidates[0].provider, negotiation.take_value()));
    } catch (const std::bad_alloc&) {
        return Result<ProcessorSelection>(allocation_failure());
    }
}

Result<EncoderSelection> BackendRegistry::select_video_encoder(
    const VideoEncoderRequest& request) const {
    try {
        const std::string& required_provider_id =
            request.constraints().required_provider_id();
        if (!required_provider_id.empty()) {
            const ProviderMap::const_iterator required =
                providers_.find(required_provider_id);
            if (required == providers_.end()) {
                return Result<EncoderSelection>(Status(
                    StatusCode::kNotFound,
                    "required backend provider is not registered: " +
                    required_provider_id));
            }
            Result<ProviderCapability> probed = required->second->probe();
            if (!probed.ok()) {
                return Result<EncoderSelection>(probed.status());
            }
            const Status matched = probed.value().match(request);
            if (!matched.ok()) {
                return Result<EncoderSelection>(matched);
            }
            Result<VideoEncoderNegotiation> negotiation =
                probed.value().negotiate(request);
            if (!negotiation.ok()) {
                return Result<EncoderSelection>(negotiation.status());
            }
            return Result<EncoderSelection>(EncoderSelection(
                required->second, negotiation.take_value()));
        }

        std::vector<EncoderCandidate> candidates;
        std::string rejections;
        for (ProviderMap::const_iterator provider = providers_.begin();
             provider != providers_.end(); ++provider) {
            Result<ProviderCapability> probed = provider->second->probe();
            if (!probed.ok()) {
                append_rejection(&rejections, provider->first,
                                 probed.status());
                continue;
            }
            const Status matched = probed.value().match(request);
            if (!matched.ok()) {
                append_rejection(&rejections, provider->first, matched);
                continue;
            }
            Result<ProviderPreferenceScore> score =
                probed.value().preference_score(request);
            if (!score.ok()) {
                return Result<EncoderSelection>(score.status());
            }
            candidates.push_back(EncoderCandidate(
                provider->second, probed.take_value(), score.take_value()));
        }
        if (candidates.empty()) {
            if (rejections.empty()) {
                rejections = "no backend providers are registered";
            }
            return Result<EncoderSelection>(capability_mismatch(
                "no video encoder provider matched: " + rejections));
        }

        std::sort(candidates.begin(), candidates.end(),
                  encoder_candidate_less);
        Result<VideoEncoderNegotiation> negotiation =
            candidates[0].capability.negotiate(request);
        if (!negotiation.ok()) {
            return Result<EncoderSelection>(negotiation.status());
        }
        return Result<EncoderSelection>(EncoderSelection(
            candidates[0].provider, negotiation.take_value()));
    } catch (const std::bad_alloc&) {
        return Result<EncoderSelection>(allocation_failure());
    }
}

}  // namespace eavp
