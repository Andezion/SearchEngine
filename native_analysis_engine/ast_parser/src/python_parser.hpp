#pragma once

#include "codegraph/language_parser.hpp"

namespace codegraph {

class PythonParser : public LanguageParser {
public:
    ParseResult parseFile(const ParseInput& input) const override;
};

}
