#ifndef EAVP_MEDIA_GRAPH_HPP_
#define EAVP_MEDIA_GRAPH_HPP_

#include <map>
#include <set>
#include <string>
#include <vector>

#include "eavp/base/result.hpp"

namespace eavp {

class MediaGraph {
public:
    Status add_node(const std::string& id);
    Status connect(const std::string& source, const std::string& sink);
    Result<std::vector<std::string> > topological_order() const;

private:
    std::vector<std::string> nodes_;
    std::set<std::string> node_set_;
    std::map<std::string, std::set<std::string> > edges_;
};

}  // namespace eavp

#endif  // EAVP_MEDIA_GRAPH_HPP_

