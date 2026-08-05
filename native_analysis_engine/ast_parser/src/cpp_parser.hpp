#pragma once

#include "codegraph/language_parser.hpp"

namespace codegraph {

class CppParser : public LanguageParser {
public:
    Graph parseFile(const ParseInput& input) const override;
};

}  
