#pragma once

#include <functional>
#include <string>
#include <vector>

namespace codegraph {

// это нужно для того, чтобы передавать в walk_directory опции обхода директорий
struct WalkOptions {
    std::vector<std::string> ignore_names; // на кого похуй, типа .git, .svn, build, out
    bool follow_symlinks = false;  // если true, то мы будем заходить в симлинки на директории, иначе нет
};

// возвращает список имён директорий/файлов, которые нужно игнорировать по умолчанию
const std::vector<std::string>& default_ignore_list();

// возвращает true, если entry_name находится в списке игнорируемых имён
bool is_ignored(const std::string& entry_name, const WalkOptions& options);

// это тип функции, которая вызывается для каждого файла/директории при обходе дерева директорий
using EntryVisitor = std::function<std::string(
    const std::string& parent_id, bool is_directory, const std::string& name, 
    const std::string& relative_path, const std::string& extension)>;

// рекурсивно обходит директорию root_absolute_path и вызывает visitor для каждого файла/директории
void walk_directory(const std::string& root_absolute_path,
                     const std::string& root_node_id, const WalkOptions& options,
                     const EntryVisitor& visitor);

}  
