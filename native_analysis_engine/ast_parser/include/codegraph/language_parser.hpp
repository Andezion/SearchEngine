#pragma once

#include <string>
#include <vector>

#include "codegraph/graph_types.hpp"

namespace codegraph {

struct ParseInput {
    std::string absolute_path;  // передаём в clang_parseTranslationUnit2
    std::string relative_path;  // это путь относительно корня проекта, нужен для генерации "defines" edge
    std::string file_node_id;   // это id узла файла в графе проекта, нужен для генерации "defines" edge
};

// Graph fragment с локальными id (как раньше) + отложенные кросс-файловые
// связи, которые parseFile() не может разрешить сама, не видя остальной
// проект - их разруливает reference_resolver после того, как весь проект
// просканирован и смержен
struct ParseResult {
    Graph fragment;
    std::vector<PendingCall> pending_calls;
    std::vector<PendingImport> pending_imports;
};

// тут мы определяем абстрактный интерфейс для парсера языка,
// который будет реализован для каждого конкретного языка
class LanguageParser {
public:
    virtual ~LanguageParser() = default;
    virtual ParseResult parseFile(const ParseInput& input) const = 0;
};

}  
