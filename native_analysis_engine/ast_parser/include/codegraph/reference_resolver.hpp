#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "codegraph/graph_types.hpp"

namespace codegraph {

void resolve_pending_references(Graph& graph, IdGenerator& id_gen,
                                 const std::vector<PendingCall>& pending_calls,
                                 const std::vector<PendingImport>& pending_imports,
                                 const std::unordered_map<std::string, std::string>& usr_to_node_id,
                                 const std::unordered_map<std::string, std::string>& path_to_file_node_id);

} 
