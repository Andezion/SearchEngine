#include "codegraph/graph_types.hpp"

namespace codegraph {

// простой генератор
std::string IdGenerator::next() {
    ++counter_;
    return std::to_string(counter_);
}

namespace {

// тут сложно, потому что nlohmann::json не умеет работать с std::optional напрямую
// поэтому мы делаем обертку, которая возвращает либо значение, либо null и тогда nlohmann::json корректно сериализует его в JSON
template <typename T>
nlohmann::json optional_to_json(const std::optional<T>& value) {
    if (value.has_value()) {
        return *value;
    }
    return nullptr;
}

} 
// просто сохраняем в JSON все поля структуры Node, Edge и Graph, используя optional_to_json для полей, которые могут быть пустыми (std::optional)
void to_json(nlohmann::json& j, const Node& n) {
    j = nlohmann::json{
        {"id", n.id},
        {"type", n.type},
        {"name", n.name},
        {"file", optional_to_json(n.file)},
        {"language", optional_to_json(n.language)},
        {"line_start", optional_to_json(n.line_start)},
        {"line_end", optional_to_json(n.line_end)},
        {"metadata", n.metadata},
    };
}

void to_json(nlohmann::json& j, const Edge& e) {
    j = nlohmann::json{
        {"id", e.id},
        {"type", e.type},
        {"source", e.source},
        {"target", e.target},
    };
}

void to_json(nlohmann::json& j, const Graph& g) {
    j = nlohmann::json{
        {"version", g.version},
        {"root", g.root},
        {"nodes", g.nodes},
        {"edges", g.edges},
    };
}

}  
