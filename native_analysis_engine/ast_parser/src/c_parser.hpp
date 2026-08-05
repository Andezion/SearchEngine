#pragma once

#include "codegraph/language_parser.hpp"

namespace codegraph {

class CParser : public LanguageParser {
public:
    Graph parseFile(const ParseInput& input) const override;
};

}  
