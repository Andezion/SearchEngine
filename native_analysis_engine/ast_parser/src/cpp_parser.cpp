#include "cpp_parser.hpp"

#include "libclang_symbol_parser.hpp"

namespace codegraph {

ParseResult CppParser::parseFile(const ParseInput& input) const {
    static const LibclangSymbolParser impl("C++", "c++", "-std=c++17");
    return impl.parseFile(input);
}

}  
