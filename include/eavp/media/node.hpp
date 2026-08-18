#ifndef EAVP_MEDIA_NODE_HPP_
#define EAVP_MEDIA_NODE_HPP_

#include <string>

#include "eavp/base/status.hpp"

namespace eavp {

enum class NodeState {
    kCreated,
    kPrepared,
    kRunning,
    kStopped,
    kError,
};

class MediaNode {
public:
    explicit MediaNode(const std::string& id);
    virtual ~MediaNode();

    const std::string& id() const;
    NodeState state() const;

    Status prepare();
    Status start();
    Status stop();
    Status reset();
    Status tick();

protected:
    virtual Status on_prepare();
    virtual Status on_start();
    virtual Status on_stop();
    virtual Status on_reset();
    virtual Status on_tick();

private:
    std::string id_;
    NodeState state_;
};

}  // namespace eavp

#endif  // EAVP_MEDIA_NODE_HPP_

