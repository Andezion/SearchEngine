#include "codegraph/directory_walker.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace codegraph {

// на что нам похуй
const std::vector<std::string>& default_ignore_list() {
    static const std::vector<std::string> kDefaults = {
        ".git",    "node_modules", "build",  "dist",  "out",
        "target",  ".vscode",      ".idea",  "__pycache__",
        "venv",    ".venv",        ".cache",
    };
    return kDefaults;
}

// проверяем, нужно ли игнорировать файл/директорию
bool is_ignored(const std::string& entry_name, const WalkOptions& options) {
    return std::find(options.ignore_names.begin(), options.ignore_names.end(),
                      entry_name) != options.ignore_names.end();
}

namespace {

// тут пиздец, мы делаем рекурсивный обход директории, вызывая visitor для каждого файла/директории
void walk_recursive(const fs::path& root, const fs::path& current,
                     const std::string& parent_id, const WalkOptions& options,
                     const EntryVisitor& visitor) {
    std::error_code ec;
    fs::directory_iterator it(current, ec);
    if (ec) {
        std::cerr << "warning: cannot read directory " << current.string() << ": "
                  << ec.message() << "\n";
        return;
    }

    std::vector<fs::directory_entry> entries;
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) {
            std::cerr << "warning: error while reading directory " << current.string()
                      << ": " << ec.message() << "\n";
            break;
        }
        entries.push_back(*it);
    }

    std::sort(entries.begin(), entries.end(),
              [](const fs::directory_entry& a, const fs::directory_entry& b) {
                  return a.path().filename().string() < b.path().filename().string();
              });

    for (const auto& entry : entries) {
        const std::string name = entry.path().filename().string();

        if (entry.is_symlink()) {
            continue;
        }
        if (is_ignored(name, options)) {
            continue;
        }

        const std::string relative_path = fs::relative(entry.path(), root).generic_string();
        const bool is_directory = entry.is_directory();
        const std::string extension = is_directory ? "" : entry.path().extension().string();

        const std::string node_id =
            visitor(parent_id, is_directory, name, relative_path, extension);

        if (is_directory) {
            walk_recursive(root, entry.path(), node_id, options, visitor);
        }
    }
}

}  

// запускаем обход директории, начиная с root_absolute_path, вызывая visitor для каждого файла/директории
void walk_directory(const std::string& root_absolute_path, const std::string& root_node_id,
                     const WalkOptions& options, const EntryVisitor& visitor) {
    fs::path root(root_absolute_path);
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        throw std::runtime_error(root_absolute_path + " is not a valid directory");
    }

    walk_recursive(root, root, root_node_id, options, visitor);
}

}  
