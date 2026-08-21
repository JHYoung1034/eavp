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
    Status status = actual_->set(state_key(), StateValue("error"));
    if (!status.ok()) {
        return status;
    }
    status = actual_->set(error_key(), StateValue(failure.message()));
    if (!status.ok()) {
        return status;
    }
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
            return actual_->erase(error_key());
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
        if (result.code() == StatusCode::kWouldBlock) {
            const Status publish =
                actual_->set(state_key(), StateValue("draining"));
            return publish.ok() ? result : publish;
        }
        return publish_failure(result);
    }
    const Status clear = actual_->erase(error_key());
    if (!clear.ok()) {
        return clear;
    }
    return actual_->set(state_key(), StateValue(desired_state.value()));
}

}  // namespace eavp
