#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace codegraph {

// эта хуйня чтобы складывать туда информацию о проекте, чтобы потом можно было строить граф
struct Node {
    std::string id; // уникальный идентификатор ноды
    std::string type; // тип ноды, например "file", "function", "class"
    std::string name; // имя ноды, например имя файла, функции или класса
    std::optional<std::string> file; // путь к файлу, если нода относится к файлу
    std::optional<std::string> language; // язык программирования, если нода относится к коду
    std::optional<int> line_start; // номер строки, с которой начинается нода, если нода относится к коду
    std::optional<int> line_end; // номер строки, на которой заканчивается нода, если нода относится к коду
    nlohmann::json metadata = nlohmann::json::object(); // дополнительные метаданные, которые могут быть полезны для анализа, например, количество строк кода, количество функций
};

// тут мы храним граф проекта, который потом можно будет сериализовать в json
struct Edge {
    std::string id; // уникальный идентификатор ребра
    std::string type; // тип ребра, например "contains", "calls", "inherits"
    std::string source; // идентификатор исходной ноды
    std::string target; // идентификатор целевой ноды
};

// наш граф, который потом можно будет сериализовать в json
struct Graph {
    std::string version = "1.0"; // просто хуйню написал, чтобы потом можно было версию графа проверять
    std::string root; // откуда идём по графу
    std::vector<Node> nodes; // список всех нод в графе
    std::vector<Edge> edges; // список всех ребер в графе
};

// тут создаём айдишки для нод и ребер, чтобы потом можно было их уникально идентифицировать
class IdGenerator {
public:
    std::string next(); // наш айди

private:
    std::uint64_t counter_ = 0;
};

struct PendingCall {
    std::string caller_usr;
    std::string callee_usr;
};

struct PendingImport {
    std::string including_file_node_id;
    std::string included_absolute_path;
};

struct PendingFieldAccess {
    std::string accessor_usr;
    std::string field_usr;
};

void to_json(nlohmann::json& j, const Node& n); // сериализуем ноду в json
void to_json(nlohmann::json& j, const Edge& e); // сериализуем ребро в json
void to_json(nlohmann::json& j, const Graph& g); // сериализуем граф в json

} 
