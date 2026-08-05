#include <filesystem>
#include <fstream>
#include <iostream>

#include "codegraph/cli_options.hpp"
#include "codegraph/directory_walker.hpp"
#include "codegraph/graph_builder.hpp"
#include "codegraph/graph_types.hpp"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    // запускаем анализатор
    codegraph::CliOptions options;
    try {
        options = codegraph::parse_cli_options(argc, argv);
    } catch (const std::invalid_argument& e) {
        std::cerr << e.what() << "\n";
        return 2;
    }

    std::error_code ec;
    // получаем абсолютный путь к корневой директории проекта
    fs::path absolute_root = fs::weakly_canonical(options.project_root, ec);
    if (ec || !fs::exists(absolute_root, ec) || !fs::is_directory(absolute_root, ec)) {
        std::cerr << "error: " << options.project_root << " is not a valid directory\n";
        return 1;
    }

    // начинаем топать по директории проекта и строить граф зависимостей
    codegraph::WalkOptions walk_options;
    walk_options.ignore_names = codegraph::default_ignore_list();
    walk_options.ignore_names.insert(walk_options.ignore_names.end(),
                                      options.extra_ignores.begin(),
                                      options.extra_ignores.end());

    codegraph::Graph graph;
    try {
        graph = codegraph::build_project_graph(absolute_root.string(), walk_options);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    nlohmann::json j = graph; // конвертируем граф в json
    const std::string dumped = j.dump(2);

    if (options.out_file) {
        std::ofstream out(*options.out_file);
        if (!out) {
            std::cerr << "error: cannot write to " << *options.out_file << "\n";
            return 1;
        }
        out << dumped << "\n";
    } else {
        std::cout << dumped << "\n";
    }

    return 0;
}
