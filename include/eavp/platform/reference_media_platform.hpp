#ifndef EAVP_PLATFORM_REFERENCE_MEDIA_PLATFORM_HPP_
#define EAVP_PLATFORM_REFERENCE_MEDIA_PLATFORM_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>

#include "eavp/control/command.hpp"
#include "eavp/control/pipeline_reconciler.hpp"
#include "eavp/management/health.hpp"
#include "eavp/management/metrics.hpp"
#include "eavp/media/backend_registry.hpp"
#include "eavp/media/executor.hpp"
#include "eavp/media/reference_backend.hpp"
#include "eavp/platform/pipeline_query.hpp"

namespace eavp {

class ReferenceMediaPlatform {
public:
    ReferenceMediaPlatform();
    explicit ReferenceMediaPlatform(const ReferenceBackendOptions& options);
    ~ReferenceMediaPlatform();

    Status initialize();
    Status dispatch(const StartPipelineCommand& command);
    Status dispatch(const StopPipelineCommand& command);
    Status reconcile_once();
    Status tick(std::size_t count);
    Status reset_pipeline();

    Result<StateSnapshot> query(const PipelineStateQuery& query) const;
    const MetricRegistry& metrics() const;
    const HealthManager& health() const;

private:
    Status build_pipeline();
    Status publish_selection(const VideoFormat& format,
                             const std::string& provider_id);
    Status publish_runtime_failure(const Status& failure);
    std::string state_key(const char* suffix = "/state") const;
    std::uint64_t encoded_count() const;

    ReferenceBackendOptions options_;
    BackendRegistry registry_;
    StateStore desired_;
    StateStore actual_;
    PipelineCommandHandler command_handler_;
    std::unique_ptr<MediaPipeline> pipeline_;
    std::unique_ptr<PipelineReconciler> reconciler_;
    MetricRegistry metrics_;
    HealthManager health_;
    DeterministicExecutor executor_;
    std::size_t source_budget_;
    bool initialized_;
};

}  // namespace eavp

#endif  // EAVP_PLATFORM_REFERENCE_MEDIA_PLATFORM_HPP_
