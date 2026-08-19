#pragma once

#include "codegraph/language_parser.hpp"

namespace codegraph {

class RustParser : public LanguageParser {
public:
    ParseResult parseFile(const ParseInput& input) const override;
};

}
