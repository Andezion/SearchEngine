#include "codegraph/reference_resolver.hpp"

#include <set>
#include <utility>

namespace codegraph {

namespace {

template <typename PendingItem>
void resolve_usr_pair_edges(Graph& graph, IdGenerator& id_gen, const std::vector<PendingItem>& items,
                             std::string PendingItem::*source_field,
                             std::string PendingItem::*target_field, const std::string& edge_type,
                             const std::unordered_map<std::string, std::string>& usr_to_node_id) {
    std::set<std::pair<std::string, std::string>> seen;  // dedup, scoped per call
    for (const PendingItem& item : items) {
        auto src = usr_to_node_id.find(item.*source_field);
        auto dst = usr_to_node_id.find(item.*target_field);
        if (src == usr_to_node_id.end() || dst == usr_to_node_id.end()) {
            continue;  // не проектный символ (stdlib и тп) - дропаем
        }
        if (!seen.emplace(src->second, dst->second).second) {
            continue;  // уже записали такую же связь
        }

        Edge edge;
        edge.id = id_gen.next();
        edge.type = edge_type;
        edge.source = src->second;
        edge.target = dst->second;
        graph.edges.push_back(std::move(edge));
    }
}

} 

void resolve_pending_references(Graph& graph, IdGenerator& id_gen,
                                 const std::vector<PendingCall>& pending_calls,
                                 const std::vector<PendingFieldAccess>& pending_reads,
                                 const std::vector<PendingFieldAccess>& pending_writes,
                                 const std::vector<PendingImport>& pending_imports,
                                 const std::vector<PendingContains>& pending_contains,
                                 const std::unordered_map<std::string, std::string>& usr_to_node_id,
                                 const std::unordered_map<std::string, std::string>& path_to_file_node_id) {
    resolve_usr_pair_edges(graph, id_gen, pending_calls, &PendingCall::caller_usr,
                            &PendingCall::callee_usr, "calls", usr_to_node_id);
    resolve_usr_pair_edges(graph, id_gen, pending_reads, &PendingFieldAccess::accessor_usr,
                            &PendingFieldAccess::field_usr, "reads", usr_to_node_id);
    resolve_usr_pair_edges(graph, id_gen, pending_writes, &PendingFieldAccess::accessor_usr,
                            &PendingFieldAccess::field_usr, "writes", usr_to_node_id);
    resolve_usr_pair_edges(graph, id_gen, pending_contains, &PendingContains::container_usr,
                            &PendingContains::member_usr, "contains", usr_to_node_id);

    std::set<std::pair<std::string, std::string>> seen_imports;
    for (const PendingImport& pi : pending_imports) {
        auto dst = path_to_file_node_id.find(pi.included_absolute_path);
        if (dst == path_to_file_node_id.end()) {
            continue;  // вне просканированного дерева (системный/сторонний заголовок) - дропаем
        }
        if (dst->second == pi.including_file_node_id) {
            continue;  // защита от самовключения
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

}  // namespace codegraph
