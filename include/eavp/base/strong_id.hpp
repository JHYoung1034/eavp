#ifndef EAVP_BASE_STRONG_ID_HPP_
#define EAVP_BASE_STRONG_ID_HPP_

#include <string>

#include "eavp/base/result.hpp"

namespace eavp {

template <typename Tag>
class StrongStringId {
public:
    static Result<StrongStringId<Tag> > create(const std::string& value) {
        if (value.empty()) {
            return Result<StrongStringId<Tag> >(
                Status(StatusCode::kInvalidArgument, "identifier must not be empty"));
        }
        return Result<StrongStringId<Tag> >(StrongStringId<Tag>(value));
    }

    const std::string& value() const { return value_; }

    bool operator==(const StrongStringId<Tag>& other) const { return value_ == other.value_; }
    bool operator!=(const StrongStringId<Tag>& other) const { return !(*this == other); }
    bool operator<(const StrongStringId<Tag>& other) const { return value_ < other.value_; }

private:
    explicit StrongStringId(const std::string& value) : value_(value) {}
    std::string value_;
};

struct PipelineIdTag {};
struct NodeIdTag {};
struct CommandIdTag {};
struct SessionIdTag {};

typedef StrongStringId<PipelineIdTag> PipelineId;
typedef StrongStringId<NodeIdTag> NodeId;
typedef StrongStringId<CommandIdTag> CommandId;
typedef StrongStringId<SessionIdTag> SessionId;

}  // namespace eavp

#endif  // EAVP_BASE_STRONG_ID_HPP_

