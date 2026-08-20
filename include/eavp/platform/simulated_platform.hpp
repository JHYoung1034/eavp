#ifndef EAVP_PLATFORM_SIMULATED_PLATFORM_HPP_
#define EAVP_PLATFORM_SIMULATED_PLATFORM_HPP_

#include <cstddef>
#include <string>

#include "eavp/control/command.hpp"
#include "eavp/control/pipeline_reconciler.hpp"
#include "eavp/management/health.hpp"
#include "eavp/management/metrics.hpp"
#include "eavp/media/executor.hpp"
#include "eavp/platform/pipeline_query.hpp"

namespace eavp {

class SimulatedPlatform {
public:
    SimulatedPlatform();

    Status initialize();
    Status dispatch(const StartPipelineCommand& command);
    Status dispatch(const StopPipelineCommand& command);
    Status reconcile_once();
    Status tick(std::size_t count);

    Result<StateSnapshot> query(const PipelineStateQuery& query) const;
    const MetricRegistry& metrics() const;
    const HealthManager& health() const;

private:
    StateStore desired_;
    StateStore actual_;
    PipelineCommandHandler command_handler_;
    MediaPipeline pipeline_;
    PipelineReconciler reconciler_;
    MetricRegistry metrics_;
    HealthManager health_;
    DeterministicExecutor executor_;
    bool initialized_;
};

}  // namespace eavp

#endif  // EAVP_PLATFORM_SIMULATED_PLATFORM_HPP_
