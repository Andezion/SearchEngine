#pragma once

#include <cstring>
#include <string>
#include <vector>

#include <tree_sitter/api.h>

namespace codegraph::ts_util {

// RAII блядский, чтобы не забыть освободить парсер/дерево, тот же паттерн что и
// ClangIndexHandle/ClangTUHandle в libclang_symbol_parser.cpp
struct ParserHandle {
    TSParser* parser;
    explicit ParserHandle(const TSLanguage* language) : parser(ts_parser_new()) {
        ts_parser_set_language(parser, language);
    }
    ~ParserHandle() { ts_parser_delete(parser); }
    ParserHandle(const ParserHandle&) = delete;
    ParserHandle& operator=(const ParserHandle&) = delete;
};

struct TreeHandle {
    TSTree* tree;
    explicit TreeHandle(TSTree* t) : tree(t) {}
    ~TreeHandle() {
        if (tree) {
            ts_tree_delete(tree);
        }
    }
    TreeHandle(const TreeHandle&) = delete;
    TreeHandle& operator=(const TreeHandle&) = delete;
};

inline bool is_null(TSNode node) { return ts_node_is_null(node); }

inline std::string node_type(TSNode node) {
    if (is_null(node)) {
        return "";
    }
    return ts_node_type(node);
}

inline std::string node_text(TSNode node, const std::string& source) {
    if (is_null(node)) {
        return "";
    }
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (end <= start || end > source.size()) {
        return "";
    }
    return source.substr(start, end - start);
}

inline TSNode field(TSNode node, const char* name) {
    return ts_node_child_by_field_name(node, name, static_cast<uint32_t>(std::strlen(name)));
}

inline std::vector<TSNode> multi_field(TSNode node, const char* name) {
    std::vector<TSNode> out;
    uint32_t n = ts_node_child_count(node);
    for (uint32_t i = 0; i < n; i++) {
        const char* fname = ts_node_field_name_for_child(node, i);
        if (fname && std::strcmp(fname, name) == 0) {
            out.push_back(ts_node_child(node, i));
        }
    }
    return out;
}

// строки у clang 1-indexed (line_start/line_end), а у tree-sitter точки
// 0-indexed - тут просто выравниваем под то, что уже принято в графе
inline int line_start(TSNode node) { return static_cast<int>(ts_node_start_point(node).row) + 1; }
inline int line_end(TSNode node) { return static_cast<int>(ts_node_end_point(node).row) + 1; }

inline std::string base_type_name(TSNode type_node, const std::string& source) {
    TSNode node = type_node;
    while (!is_null(node)) {
        std::string kind = node_type(node);
        if (kind == "pointer_type" || kind == "reference_type") {
            uint32_t n = ts_node_named_child_count(node);
            if (n == 0) {
                break;
            }
            node = ts_node_named_child(node, 0);
            continue;
        }
        if (kind == "generic_type") {
            TSNode inner = field(node, "type");
            if (!is_null(inner)) {
                node = inner;
                continue;
            }
        }
        if (kind == "qualified_type") {
            TSNode name = field(node, "name");
            if (!is_null(name)) {
                node = name;
                continue;
            }
        }
        if (kind == "scoped_type_identifier") {
            TSNode name = field(node, "name");
            if (!is_null(name)) {
                node = name;
                continue;
            }
        }
        break;
    }
    return node_text(node, source);
}

} 
