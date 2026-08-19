#pragma once

#include "codegraph/language_parser.hpp"

namespace codegraph {

class GoParser : public LanguageParser {
public:
    ParseResult parseFile(const ParseInput& input) const override;
};

}
