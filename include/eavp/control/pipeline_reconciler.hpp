#ifndef EAVP_CONTROL_PIPELINE_RECONCILER_HPP_
#define EAVP_CONTROL_PIPELINE_RECONCILER_HPP_

#include "eavp/control/state_store.hpp"
#include "eavp/media/pipeline.hpp"

namespace eavp {

class PipelineReconciler {
public:
    PipelineReconciler(MediaPipeline* pipeline, StateStore* desired, StateStore* actual);

    Status reconcile_once();

private:
    std::string state_key() const;
    std::string error_key() const;
    Status publish_failure(const Status& failure);

    MediaPipeline* pipeline_;
    StateStore* desired_;
    StateStore* actual_;
};

}  // namespace eavp

#endif  // EAVP_CONTROL_PIPELINE_RECONCILER_HPP_

