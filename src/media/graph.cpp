#include "eavp/media/graph.hpp"

#include <cstddef>
#include <map>
#include <queue>

namespace eavp {

Status MediaGraph::add_node(const std::string& id) {
    if (id.empty()) {
        return Status(StatusCode::kInvalidArgument, "node id must not be empty");
    }
    if (!node_set_.insert(id).second) {
        return Status(StatusCode::kAlreadyExists, "node already exists");
    }
    nodes_.push_back(id);
    return Status::ok_status();
}

Status MediaGraph::connect(const std::string& source, const std::string& sink) {
    if (node_set_.count(source) == 0U || node_set_.count(sink) == 0U) {
        return Status(StatusCode::kNotFound, "graph endpoint does not exist");
    }
    if (source == sink) {
        return Status(StatusCode::kInvalidArgument, "self edge is not allowed");
    }
    if (!edges_[source].insert(sink).second) {
        return Status(StatusCode::kAlreadyExists, "edge already exists");
    }
    if (!topological_order().ok()) {
        edges_[source].erase(sink);
        return Status(StatusCode::kInvalidArgument, "edge would create a cycle");
    }
    return Status::ok_status();
}

Result<std::vector<std::string> > MediaGraph::topological_order() const {
    std::map<std::string, std::size_t> indegree;
    for (std::vector<std::string>::const_iterator it = nodes_.begin(); it != nodes_.end(); ++it) {
        indegree[*it] = 0U;
    }
    for (std::map<std::string, std::set<std::string> >::const_iterator edge = edges_.begin();
         edge != edges_.end(); ++edge) {
        for (std::set<std::string>::const_iterator sink = edge->second.begin();
             sink != edge->second.end(); ++sink) {
            ++indegree[*sink];
        }
    }

    std::queue<std::string> ready;
    for (std::vector<std::string>::const_iterator it = nodes_.begin(); it != nodes_.end(); ++it) {
        if (indegree[*it] == 0U) {
            ready.push(*it);
        }
    }

    std::vector<std::string> order;
    while (!ready.empty()) {
        const std::string current = ready.front();
        ready.pop();
        order.push_back(current);
        const std::map<std::string, std::set<std::string> >::const_iterator outgoing =
            edges_.find(current);
        if (outgoing == edges_.end()) {
            continue;
        }
        for (std::set<std::string>::const_iterator sink = outgoing->second.begin();
             sink != outgoing->second.end(); ++sink) {
            --indegree[*sink];
            if (indegree[*sink] == 0U) {
                ready.push(*sink);
            }
        }
    }

    if (order.size() != nodes_.size()) {
        return Result<std::vector<std::string> >(
            Status(StatusCode::kInvalidArgument, "graph contains a cycle"));
    }
    return Result<std::vector<std::string> >(order);
}

}  // namespace eavp

