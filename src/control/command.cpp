#include "eavp/control/command.hpp"

namespace eavp {

PipelineCommandHandler::PipelineCommandHandler(StateStore* desired) : desired_(desired) {}

Status PipelineCommandHandler::set_desired(const CommandHeader& header,
                                           const std::string& pipeline_id, const char* state) {
    if (desired_ == NULL) {
        return Status(StatusCode::kInvalidState, "desired state store is not configured");
    }
    if (header.id.empty() || header.source.empty() || pipeline_id.empty()) {
        return Status(StatusCode::kInvalidArgument, "command fields must not be empty");
    }
    return desired_->set("/pipelines/" + pipeline_id + "/state", StateValue(state));
}

Status PipelineCommandHandler::handle(const StartPipelineCommand& command) {
    return set_desired(command.header, command.pipeline_id, "running");
}

Status PipelineCommandHandler::handle(const StopPipelineCommand& command) {
    return set_desired(command.header, command.pipeline_id, "stopped");
}

}  // namespace eavp

