#ifndef EAVP_MEDIA_EXECUTOR_HPP_
#define EAVP_MEDIA_EXECUTOR_HPP_

#include <cstddef>

#include "eavp/media/pipeline.hpp"

namespace eavp {

class DeterministicExecutor {
public:
    Status run(MediaPipeline* pipeline, std::size_t ticks) const {
        if (pipeline == NULL) {
            return Status(StatusCode::kInvalidArgument, "pipeline must not be null");
        }
        for (std::size_t index = 0U; index < ticks; ++index) {
            const Status status = pipeline->tick();
            if (!status.ok()) {
                return status;
            }
        }
        return Status::ok_status();
    }
};

}  // namespace eavp

#endif  // EAVP_MEDIA_EXECUTOR_HPP_

