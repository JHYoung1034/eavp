#ifndef EAVP_MEDIA_PIPELINE_HPP_
#define EAVP_MEDIA_PIPELINE_HPP_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "eavp/media/graph.hpp"
#include "eavp/media/node.hpp"

namespace eavp {

enum class PipelineState {
    kCreated,
    kRunning,
    kStopped,
    kError,
};

class MediaPipeline {
public:
    explicit MediaPipeline(const std::string& id);
    ~MediaPipeline();

    const std::string& id() const;
    PipelineState state() const;

    Status add_node(std::unique_ptr<MediaNode> node);
    Status connect(const std::string& source, const std::string& sink);
    Status start();
    Status stop();
    Status tick();

private:
    std::vector<MediaNode*> ordered_nodes(const std::vector<std::string>& order) const;
    void stop_nodes_reverse(const std::vector<MediaNode*>& nodes);

    std::string id_;
    PipelineState state_;
    MediaGraph graph_;
    std::vector<std::unique_ptr<MediaNode> > nodes_;
    std::map<std::string, MediaNode*> nodes_by_id_;
};

}  // namespace eavp

#endif  // EAVP_MEDIA_PIPELINE_HPP_

