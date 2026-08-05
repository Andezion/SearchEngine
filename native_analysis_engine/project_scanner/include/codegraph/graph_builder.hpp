#pragma once

#include <string>

#include "codegraph/directory_walker.hpp"
#include "codegraph/graph_types.hpp"

namespace codegraph {

// это нужно чтобы не было конфликта с graph_types.hpp, где уже есть Graph
Graph build_project_graph(const std::string& project_root_absolute_path,
                           const WalkOptions& walk_options);

}  
