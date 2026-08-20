#ifndef EAVP_PLATFORM_PIPELINE_QUERY_HPP_
#define EAVP_PLATFORM_PIPELINE_QUERY_HPP_

#include <string>

namespace eavp {

struct PipelineStateQuery {
    explicit PipelineStateQuery(const std::string& pipeline)
        : pipeline_id(pipeline) {}

    std::string pipeline_id;
};

}  // namespace eavp

#endif  // EAVP_PLATFORM_PIPELINE_QUERY_HPP_
