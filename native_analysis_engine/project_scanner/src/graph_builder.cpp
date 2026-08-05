#include "codegraph/graph_builder.hpp"

#include <filesystem>
#include <iostream>

#include "codegraph/graph_merge.hpp"
#include "codegraph/parser_registry.hpp"

namespace codegraph {

// строим графы тут
Graph build_project_graph(const std::string& project_root_absolute_path,
                           const WalkOptions& walk_options) {
    // начало нашего графа
    Graph graph;
    // ну и путь к проекту самому
    graph.root = project_root_absolute_path;

    IdGenerator id_gen; // хуйня айди

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
                    Graph fragment = parser->parseFile(input);
                    merge_graph_fragment(graph, std::move(fragment), id_gen);
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

    return graph;
}

} 
