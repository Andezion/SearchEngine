#include "codegraph/graph_builder.hpp"

#include <filesystem>
#include <iostream>
#include <unordered_map>

#include "codegraph/graph_merge.hpp"
#include "codegraph/parser_registry.hpp"
#include "codegraph/reference_resolver.hpp"

namespace codegraph {

// строим графы тут
Graph build_project_graph(const std::string& project_root_absolute_path,
                           const WalkOptions& walk_options) {
    // начало нашего графа
    Graph graph;
    // ну и путь к проекту самому
    graph.root = project_root_absolute_path;

    IdGenerator id_gen; // хуйня айди

    // отложенные кросс-файловые связи - резолвятся после того, как весь
    // проект просканирован (см. reference_resolver.hpp)
    std::vector<PendingCall> all_pending_calls;
    std::vector<PendingImport> all_pending_imports;
    std::vector<PendingFieldAccess> all_pending_reads;
    std::vector<PendingFieldAccess> all_pending_writes;
    std::vector<PendingContains> all_pending_contains;

    // создаём узел
    Node project_node; 
    project_node.id = id_gen.next();
    project_node.type = "project";
    project_node.name = std::filesystem::path(project_root_absolute_path).filename().string();
    if (project_node.name.empty()) {
        project_node.name = project_root_absolute_path;
    }
    graph.nodes.push_back(project_node); // добавляем его в граф

    const std::string project_id = project_node.id;

    // создаём visitor, который будет вызываться для каждого файла/директории, это у нас лямбда-дура
    EntryVisitor visitor = [&](const std::string& parent_id, bool is_directory,
                                const std::string& name, const std::string& relative_path,
                                const std::string& extension) -> std::string {
        // создаём узел
        Node node;
        node.id = id_gen.next();
        node.type = is_directory ? "directory" : "file";
        node.name = name;
        node.file = relative_path;
        if (!is_directory) {
            node.metadata["extension"] = extension;
            node.language = language_for_extension(extension);
        }
        graph.nodes.push_back(node);
        const std::string node_id = node.id;
        // создаём ребро
        Edge edge;
        edge.id = id_gen.next();
        edge.type = "contains";
        edge.source = parent_id;
        edge.target = node_id;
        graph.edges.push_back(edge);

        if (!is_directory) {
            if (auto parser = parser_for_extension(extension)) {
                try {
                    ParseInput input{
                        (std::filesystem::path(project_root_absolute_path) / relative_path)
                            .string(),
                        relative_path,
                        node_id,
                    };
                    ParseResult result = parser->parseFile(input);
                    merge_graph_fragment(graph, std::move(result.fragment), id_gen);
                    all_pending_calls.insert(all_pending_calls.end(),
                                              std::make_move_iterator(result.pending_calls.begin()),
                                              std::make_move_iterator(result.pending_calls.end()));
                    all_pending_imports.insert(
                        all_pending_imports.end(), std::make_move_iterator(result.pending_imports.begin()),
                        std::make_move_iterator(result.pending_imports.end()));
                    all_pending_reads.insert(all_pending_reads.end(),
                                              std::make_move_iterator(result.pending_reads.begin()),
                                              std::make_move_iterator(result.pending_reads.end()));
                    all_pending_writes.insert(all_pending_writes.end(),
                                               std::make_move_iterator(result.pending_writes.begin()),
                                               std::make_move_iterator(result.pending_writes.end()));
                    all_pending_contains.insert(
                        all_pending_contains.end(), std::make_move_iterator(result.pending_contains.begin()),
                        std::make_move_iterator(result.pending_contains.end()));
                } catch (const std::exception& e) {
                    std::cerr << "warning: symbol extraction failed for " << relative_path
                              << ": " << e.what() << " (file node kept, symbols skipped)\n";
                }
            }
        }

        return node_id;
    };

    // запускаем обход директории
    walk_directory(project_root_absolute_path, project_id, walk_options, visitor);

    // теперь, когда все узлы получили финальные глобальные id, строим
    // справочники и резолвим отложенные кросс-файловые связи (calls/imports)
    std::unordered_map<std::string, std::string> usr_to_node_id;
    std::unordered_map<std::string, std::string> path_to_file_node_id;
    for (const Node& node : graph.nodes) {
        if (node.metadata.contains("usr")) {
            const std::string usr = node.metadata.at("usr").get<std::string>();
            if (!usr.empty()) {
                usr_to_node_id.emplace(usr, node.id);  // first-wins при дублях
            }
        }
        // и файлы (для #include/use/import на конкретный файл), и
        // директории (Go импортирует пакет целиком - каталог, а не файл)
        if ((node.type == "file" || node.type == "directory") && node.file) {
            std::error_code ec;
            std::filesystem::path abs = std::filesystem::weakly_canonical(
                std::filesystem::path(project_root_absolute_path) / *node.file, ec);
            if (!ec) {
                path_to_file_node_id.emplace(abs.string(), node.id);
            }
        }
    }
    resolve_pending_references(graph, id_gen, all_pending_calls, all_pending_reads, all_pending_writes,
                                all_pending_imports, all_pending_contains, usr_to_node_id,
                                path_to_file_node_id);

    return graph;
}

} 
