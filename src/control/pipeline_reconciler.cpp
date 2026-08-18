#include "eavp/control/pipeline_reconciler.hpp"

namespace eavp {

PipelineReconciler::PipelineReconciler(MediaPipeline* pipeline, StateStore* desired,
                                       StateStore* actual)
    : pipeline_(pipeline), desired_(desired), actual_(actual) {}

std::string PipelineReconciler::state_key() const {
    return "/pipelines/" + pipeline_->id() + "/state";
}

std::string PipelineReconciler::error_key() const {
    return "/pipelines/" + pipeline_->id() + "/error";
}

Status PipelineReconciler::publish_failure(const Status& failure) {
    actual_->set(state_key(), StateValue("error"));
    actual_->set(error_key(), StateValue(failure.message()));
    return failure;
}

Status PipelineReconciler::reconcile_once() {
    if (pipeline_ == NULL || desired_ == NULL || actual_ == NULL) {
        return Status(StatusCode::kInvalidState, "reconciler dependencies are not configured");
    }
    const Result<StateValue> desired_value = desired_->snapshot().get(state_key());
    if (!desired_value.ok()) {
        return desired_value.status();
    }
    const Result<std::string> desired_state = desired_value.value().as_string();
    if (!desired_state.ok()) {
        return desired_state.status();
    }

    const Result<StateValue> actual_value = actual_->snapshot().get(state_key());
    if (actual_value.ok()) {
        const Result<std::string> actual_state = actual_value.value().as_string();
        if (actual_state.ok() && actual_state.value() == desired_state.value()) {
            return Status::ok_status();
        }
    }

    Status result;
    if (desired_state.value() == "running") {
        result = pipeline_->start();
    } else if (desired_state.value() == "stopped") {
        result = pipeline_->stop();
    } else {
        return Status(StatusCode::kInvalidArgument, "unsupported desired pipeline state");
    }
    if (!result.ok()) {
        return publish_failure(result);
    }
    return actual_->set(state_key(), StateValue(desired_state.value()));
}

}  // namespace eavp

