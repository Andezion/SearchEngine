#pragma once

#include <optional>
#include <string>
#include <vector>

namespace codegraph {

struct CliOptions {
    std::string project_root; // нужная директория, в которой лежит проект
    std::optional<std::string> out_file; // --out <file>
    std::vector<std::string> extra_ignores; // --ignore <name>, repeatable
};

// выкидывает исключение std::runtime_error, если что-то не так с аргументами командной строки
CliOptions parse_cli_options(int argc, char** argv);
// возвращает текст с инструкцией по использованию программы, который выводится при --help
std::string usage_text(const char* program_name);

}  
