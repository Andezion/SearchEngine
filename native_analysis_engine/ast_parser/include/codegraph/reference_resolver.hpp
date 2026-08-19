#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "codegraph/graph_types.hpp"

namespace codegraph {

void resolve_pending_references(Graph& graph, IdGenerator& id_gen,
                                 const std::vector<PendingCall>& pending_calls,
                                 const std::vector<PendingFieldAccess>& pending_reads,
                                 const std::vector<PendingFieldAccess>& pending_writes,
                                 const std::vector<PendingImport>& pending_imports,
                                 const std::vector<PendingContains>& pending_contains,
                                 const std::unordered_map<std::string, std::string>& usr_to_node_id,
                                 const std::unordered_map<std::string, std::string>& path_to_file_node_id);

} 
