#include "c_parser.hpp"

#include "libclang_symbol_parser.hpp"

namespace codegraph {

ParseResult CParser::parseFile(const ParseInput& input) const {
    static const LibclangSymbolParser impl("C", "c", "-std=c17");
    return impl.parseFile(input);
}

}  
