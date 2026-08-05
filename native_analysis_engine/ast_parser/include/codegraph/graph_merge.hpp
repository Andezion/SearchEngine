#pragma once

#include "codegraph/graph_types.hpp"

namespace codegraph {

// передаём в target граф, который был получен из parseFile() и который нужно влить в target
void merge_graph_fragment(Graph& target, Graph fragment, IdGenerator& id_gen);

} 
