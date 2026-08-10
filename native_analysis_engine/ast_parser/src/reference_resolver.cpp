#include "codegraph/reference_resolver.hpp"

#include <set>
#include <utility>

namespace codegraph {

void resolve_pending_references(Graph& graph, IdGenerator& id_gen,
                                 const std::vector<PendingCall>& pending_calls,
                                 const std::vector<PendingImport>& pending_imports,
                                 const std::unordered_map<std::string, std::string>& usr_to_node_id,
                                 const std::unordered_map<std::string, std::string>& path_to_file_node_id) {
    std::set<std::pair<std::string, std::string>> seen_calls;
    for (const PendingCall& pc : pending_calls) {
        auto src = usr_to_node_id.find(pc.caller_usr);
        auto dst = usr_to_node_id.find(pc.callee_usr);
        if (src == usr_to_node_id.end() || dst == usr_to_node_id.end()) {
            continue; 
        }
        if (!seen_calls.emplace(src->second, dst->second).second) {
            continue;  
        }

        Edge edge;
        edge.id = id_gen.next();
        edge.type = "calls";
        edge.source = src->second;
        edge.target = dst->second;
        graph.edges.push_back(std::move(edge));
    }

    std::set<std::pair<std::string, std::string>> seen_imports;
    for (const PendingImport& pi : pending_imports) {
        auto dst = path_to_file_node_id.find(pi.included_absolute_path);
        if (dst == path_to_file_node_id.end()) {
            continue;  
        }
        if (dst->second == pi.including_file_node_id) {
            continue;  
        }
        if (!seen_imports.emplace(pi.including_file_node_id, dst->second).second) {
            continue;
        }

        Edge edge;
        edge.id = id_gen.next();
        edge.type = "imports";
        edge.source = pi.including_file_node_id;
        edge.target = dst->second;
        graph.edges.push_back(std::move(edge));
    }
}

} 
