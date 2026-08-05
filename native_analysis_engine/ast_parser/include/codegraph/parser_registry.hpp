#pragma once

#include <memory>
#include <optional>
#include <string>

#include "codegraph/language_parser.hpp"

namespace codegraph {

// пытаемся понять что за язык по расширению файла и возвращаем соответствующий LanguageParser
std::unique_ptr<LanguageParser> parser_for_extension(const std::string& extension);

// проверяем что за язык по расширению файла и возвращаем его название 
std::optional<std::string> language_for_extension(const std::string& extension);

}  
