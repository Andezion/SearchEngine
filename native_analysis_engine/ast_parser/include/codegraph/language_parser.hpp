#pragma once

#include <string>

#include "codegraph/graph_types.hpp"

namespace codegraph {

struct ParseInput {
    std::string absolute_path;  // передаём в clang_parseTranslationUnit2
    std::string relative_path;  // это путь относительно корня проекта, нужен для генерации "defines" edge
    std::string file_node_id;   // это id узла файла в графе проекта, нужен для генерации "defines" edge
};

// тут мы определяем абстрактный интерфейс для парсера языка, 
// который будет реализован для каждого конкретного языка
class LanguageParser {
public:
    virtual ~LanguageParser() = default;
    virtual Graph parseFile(const ParseInput& input) const = 0;
};

}  
