#include "codegraph/graph_merge.hpp"

#include <unordered_map>

namespace codegraph {

// тут мы сливаем граф fragment в граф target, 
// при этом все id узлов в fragment заменяются на новые уникальные id, 
// а связи (edges) обновляются соответственно
// 
// если edge ссылается на id, которого нет в fragment, то он остаётся без изменений
void merge_graph_fragment(Graph& target, Graph fragment, IdGenerator& id_gen) {
    std::unordered_map<std::string, std::string> id_map;
    id_map.reserve(fragment.nodes.size());

    for (Node& node : fragment.nodes) {
        const std::string new_id = id_gen.next();
        id_map[node.id] = new_id;
        node.id = new_id;
        target.nodes.push_back(std::move(node));
    }

    for (Edge& edge : fragment.edges) {
        edge.id = id_gen.next();

        auto source_it = id_map.find(edge.source);
        if (source_it != id_map.end()) {
            edge.source = source_it->second;
        }
        auto target_it = id_map.find(edge.target);
        if (target_it != id_map.end()) {
            edge.target = target_it->second;
        }

        target.edges.push_back(std::move(edge));
    }
}

}  
