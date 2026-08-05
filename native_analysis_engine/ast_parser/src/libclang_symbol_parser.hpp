#pragma once

#include <string>

#include "codegraph/language_parser.hpp"

namespace codegraph {

// эта хуйня нужна чтобы парсить C/C++ код с помощью libclang, 
// потому что у него нет нормального апи для получения AST в виде графа
class LibclangSymbolParser {
public:
    LibclangSymbolParser(std::string language_label, std::string clang_x_mode,
                          std::string clang_std_flag);

    Graph parseFile(const ParseInput& input) const;

private:
    std::string language_label_;  // "C" / "C++" идёт в Node::language
    std::string clang_x_mode_;  // "c" / "c++" будет "-x" <mode>
    std::string clang_std_flag_;  // "-std=c17" / "-std=c++17"
};

}  
