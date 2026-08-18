#ifndef EAVP_CONTROL_COMMAND_HPP_
#define EAVP_CONTROL_COMMAND_HPP_

#include <string>

#include "eavp/control/state_store.hpp"

namespace eavp {

struct CommandHeader {
    CommandHeader(const std::string& command_id, const std::string& command_source)
        : id(command_id), source(command_source) {}

    std::string id;
    std::string source;
};

struct StartPipelineCommand {
    StartPipelineCommand(const std::string& id, const std::string& source,
                         const std::string& pipeline)
        : header(id, source), pipeline_id(pipeline) {}

    CommandHeader header;
    std::string pipeline_id;
};

struct StopPipelineCommand {
    StopPipelineCommand(const std::string& id, const std::string& source,
                        const std::string& pipeline)
        : header(id, source), pipeline_id(pipeline) {}

    CommandHeader header;
    std::string pipeline_id;
};

class PipelineCommandHandler {
public:
    explicit PipelineCommandHandler(StateStore* desired);

    Status handle(const StartPipelineCommand& command);
    Status handle(const StopPipelineCommand& command);

private:
    Status set_desired(const CommandHeader& header, const std::string& pipeline_id,
                       const char* state);

    StateStore* desired_;
};

}  // namespace eavp

#endif  // EAVP_CONTROL_COMMAND_HPP_

