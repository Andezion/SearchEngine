#include "rust_parser.hpp"

#include <tree_sitter/api.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "tree_sitter_util.hpp"

extern "C" const TSLanguage* tree_sitter_rust(void);

namespace codegraph {

namespace {

namespace fs = std::filesystem;
using namespace ts_util;

enum class Mode { Read, Write };

std::string qualify(const std::string& prefix, const std::string& name) {
    return prefix.empty() ? name : prefix + "::" + name;
}

struct VisitContext {
    Graph* fragment;
    IdGenerator* local_ids;
    const ParseInput* input;
    const std::string* source;
    std::string container_node_id;  // File либо Namespace(mod), куда идут defines/fallback-контейнер
    std::string qualifier;          // путь вложенных mod-ов для построения usr
    std::vector<PendingCall>* pending_calls;
    std::vector<PendingFieldAccess>* pending_reads;
    std::vector<PendingFieldAccess>* pending_writes;
    std::vector<PendingContains>* pending_contains;
    std::vector<PendingImport>* pending_imports;
};

void fill_line_range(TSNode node, Node& out) {
    out.line_start = line_start(node);
    out.line_end = line_end(node);
}

nlohmann::json collect_parameters(TSNode parameters, const std::string& source) {
    nlohmann::json params = nlohmann::json::array();
    if (is_null(parameters)) {
        return params;
    }
    uint32_t n = ts_node_named_child_count(parameters);
    for (uint32_t i = 0; i < n; i++) {
        TSNode p = ts_node_named_child(parameters, i);
        std::string kind = node_type(p);
        if (kind == "self_parameter") {
            continue;
        }
        if (kind != "parameter") {
            continue;
        }
        TSNode pattern = field(p, "pattern");
        if (node_type(pattern) == "self") {
            continue;
        }
        nlohmann::json param;
        param["name"] = node_text(pattern, source);
        param["type"] = node_text(field(p, "type"), source);
        params.push_back(std::move(param));
    }
    return params;
}

bool has_self_receiver(TSNode parameters) {
    if (is_null(parameters)) {
        return false;
    }
    uint32_t n = ts_node_named_child_count(parameters);
    if (n == 0) {
        return false;
    }
    TSNode first = ts_node_named_child(parameters, 0);
    std::string kind = node_type(first);
    if (kind == "self_parameter") {
        return true;
    }
    if (kind == "parameter" && node_type(field(first, "pattern")) == "self") {
        return true;
    }
    return false;
}

void walk_body(TSNode node, Mode mode, const std::string& owner_usr, const std::string& self_type_qualified,
               const std::string& source, std::vector<PendingCall>* calls,
               std::vector<PendingFieldAccess>* reads, std::vector<PendingFieldAccess>* writes) {
    if (is_null(node)) {
        return;
    }
    std::string kind = node_type(node);

    if (kind == "assignment_expression" || kind == "compound_assignment_expr") {
        walk_body(field(node, "left"), Mode::Write, owner_usr, self_type_qualified, source, calls, reads, writes);
        walk_body(field(node, "right"), Mode::Read, owner_usr, self_type_qualified, source, calls, reads, writes);
        return;
    }

    if (kind == "call_expression") {
        TSNode fn = field(node, "function");
        std::string fn_kind = node_type(fn);
        std::optional<std::string> callee_usr;
        if (fn_kind == "identifier") {
            callee_usr = "fn:" + node_text(fn, source);
        } else if (fn_kind == "field_expression") {
            TSNode value = field(fn, "value");
            if (node_type(value) == "self" && !self_type_qualified.empty()) {
                callee_usr = "fn:" + qualify(self_type_qualified, node_text(field(fn, "field"), source));
            }
        } else if (fn_kind == "scoped_identifier") {
            callee_usr = "fn:" + node_text(field(fn, "name"), source);
        }
        if (callee_usr && !owner_usr.empty()) {
            calls->push_back(PendingCall{owner_usr, *callee_usr});
        }
        if (fn_kind == "field_expression") {
            walk_body(field(fn, "value"), Mode::Read, owner_usr, self_type_qualified, source, calls, reads, writes);
        } else {
            walk_body(fn, Mode::Read, owner_usr, self_type_qualified, source, calls, reads, writes);
        }
        walk_body(field(node, "arguments"), Mode::Read, owner_usr, self_type_qualified, source, calls, reads,
                  writes);
        return;
    }

    if (kind == "field_expression") {
        TSNode value = field(node, "value");
        if (node_type(value) == "self" && !self_type_qualified.empty()) {
            std::string field_usr = "field:" + qualify(self_type_qualified, node_text(field(node, "field"), source));
            if (mode == Mode::Write) {
                writes->push_back(PendingFieldAccess{owner_usr, field_usr});
            } else {
                reads->push_back(PendingFieldAccess{owner_usr, field_usr});
            }
            return;  // self - лист, дальше нечего обходить
        }
        walk_body(value, Mode::Read, owner_usr, self_type_qualified, source, calls, reads, writes);
        return;
    }

    uint32_t n = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < n; i++) {
        walk_body(ts_node_named_child(node, i), mode, owner_usr, self_type_qualified, source, calls, reads, writes);
    }
}

void add_field(TSNode field_node, const std::string& parent_id, const std::string& type_qualified,
               VisitContext& ctx) {
    const std::string& source = *ctx.source;
    Node n;
    n.id = ctx.local_ids->next();
    n.type = "field";
    n.name = node_text(field(field_node, "name"), source);
    n.file = ctx.input->relative_path;
    n.language = "Rust";
    fill_line_range(field_node, n);
    n.metadata["field_type"] = node_text(field(field_node, "type"), source);
    n.metadata["usr"] = "field:" + qualify(type_qualified, n.name);
    ctx.fragment->nodes.push_back(n);

    Edge e;
    e.id = ctx.local_ids->next();
    e.type = "contains";
    e.source = parent_id;
    e.target = n.id;
    ctx.fragment->edges.push_back(e);
}

void handle_struct(TSNode node, VisitContext& ctx) {
    const std::string& source = *ctx.source;
    std::string name = node_text(field(node, "name"), source);

    Node n;
    n.id = ctx.local_ids->next();
    n.type = "struct";
    n.name = name;
    n.file = ctx.input->relative_path;
    n.language = "Rust";
    fill_line_range(node, n);
    std::string type_qualified = qualify(ctx.qualifier, name);
    n.metadata["usr"] = "type:" + type_qualified;
    ctx.fragment->nodes.push_back(n);
    std::string struct_id = n.id;

    Edge def;
    def.id = ctx.local_ids->next();
    def.type = "defines";
    def.source = ctx.container_node_id;
    def.target = struct_id;
    ctx.fragment->edges.push_back(def);

    TSNode body = field(node, "body");
    if (!is_null(body) && node_type(body) == "field_declaration_list") {
        uint32_t cnt = ts_node_named_child_count(body);
        for (uint32_t i = 0; i < cnt; i++) {
            TSNode f = ts_node_named_child(body, i);
            if (node_type(f) == "field_declaration") {
                add_field(f, struct_id, type_qualified, ctx);
            }
        }
    }
}

void handle_enum(TSNode node, VisitContext& ctx) {
    const std::string& source = *ctx.source;
    std::string name = node_text(field(node, "name"), source);

    Node n;
    n.id = ctx.local_ids->next();
    n.type = "enum";
    n.name = name;
    n.file = ctx.input->relative_path;
    n.language = "Rust";
    fill_line_range(node, n);
    n.metadata["usr"] = "type:" + qualify(ctx.qualifier, name);
    ctx.fragment->nodes.push_back(n);
    std::string enum_id = n.id;

    Edge def;
    def.id = ctx.local_ids->next();
    def.type = "defines";
    def.source = ctx.container_node_id;
    def.target = enum_id;
    ctx.fragment->edges.push_back(def);

    TSNode body = field(node, "body");
    if (is_null(body)) {
        return;
    }
    uint32_t cnt = ts_node_named_child_count(body);
    for (uint32_t i = 0; i < cnt; i++) {
        TSNode variant = ts_node_named_child(body, i);
        if (node_type(variant) != "enum_variant") {
            continue;
        }
        Node cn;
        cn.id = ctx.local_ids->next();
        cn.type = "constant";
        cn.name = node_text(field(variant, "name"), source);
        cn.file = ctx.input->relative_path;
        cn.language = "Rust";
        fill_line_range(variant, cn);
        TSNode value = field(variant, "value");
        if (!is_null(value)) {
            cn.metadata["value"] = node_text(value, source);
        }
        ctx.fragment->nodes.push_back(cn);

        Edge e;
        e.id = ctx.local_ids->next();
        e.type = "contains";
        e.source = enum_id;
        e.target = cn.id;
        ctx.fragment->edges.push_back(e);
    }
}

void add_impl_method(TSNode fn_node, const std::string& impl_type_qualified, VisitContext& ctx) {
    const std::string& source = *ctx.source;
    Node n;
    n.id = ctx.local_ids->next();
    n.type = "method";
    n.name = node_text(field(fn_node, "name"), source);
    n.file = ctx.input->relative_path;
    n.language = "Rust";
    fill_line_range(fn_node, n);
    std::string usr = "fn:" + qualify(impl_type_qualified, n.name);
    n.metadata["usr"] = usr;
    n.metadata["parameters"] = collect_parameters(field(fn_node, "parameters"), source);
    TSNode ret = field(fn_node, "return_type");
    if (!is_null(ret)) {
        n.metadata["return_type"] = node_text(ret, source);
    }
    ctx.fragment->nodes.push_back(n);

    Edge def;
    def.id = ctx.local_ids->next();
    def.type = "defines";
    def.source = ctx.container_node_id;
    def.target = n.id;
    ctx.fragment->edges.push_back(def);

    ctx.pending_contains->push_back(PendingContains{"type:" + impl_type_qualified, usr});

    bool self_recv = has_self_receiver(field(fn_node, "parameters"));
    walk_body(field(fn_node, "body"), Mode::Read, usr, self_recv ? impl_type_qualified : "", source,
              ctx.pending_calls, ctx.pending_reads, ctx.pending_writes);
}

void handle_impl(TSNode node, VisitContext& ctx) {
    const std::string& source = *ctx.source;
    TSNode type_field = field(node, "type");
    if (is_null(type_field)) {
        return;
    }
    std::string impl_type_qualified = qualify(ctx.qualifier, base_type_name(type_field, source));

    TSNode body = field(node, "body");
    if (is_null(body)) {
        return;
    }
    uint32_t n = ts_node_named_child_count(body);
    for (uint32_t i = 0; i < n; i++) {
        TSNode member = ts_node_named_child(body, i);
        if (node_type(member) == "function_item") {
            add_impl_method(member, impl_type_qualified, ctx);
        }
    }
}

void handle_trait(TSNode node, VisitContext& ctx) {
    const std::string& source = *ctx.source;
    std::string name = node_text(field(node, "name"), source);

    Node n;
    n.id = ctx.local_ids->next();
    n.type = "interface";
    n.name = name;
    n.file = ctx.input->relative_path;
    n.language = "Rust";
    fill_line_range(node, n);
    std::string trait_qualified = qualify(ctx.qualifier, name);
    n.metadata["usr"] = "type:" + trait_qualified;
    ctx.fragment->nodes.push_back(n);
    std::string trait_id = n.id;

    Edge def;
    def.id = ctx.local_ids->next();
    def.type = "defines";
    def.source = ctx.container_node_id;
    def.target = trait_id;
    ctx.fragment->edges.push_back(def);

    TSNode body = field(node, "body");
    if (is_null(body)) {
        return;
    }
    uint32_t cnt = ts_node_named_child_count(body);
    for (uint32_t i = 0; i < cnt; i++) {
        TSNode member = ts_node_named_child(body, i);
        std::string kind = node_type(member);
        if (kind != "function_item" && kind != "function_signature_item") {
            continue;
        }
        Node mn;
        mn.id = ctx.local_ids->next();
        mn.type = "method";
        mn.name = node_text(field(member, "name"), source);
        mn.file = ctx.input->relative_path;
        mn.language = "Rust";
        fill_line_range(member, mn);
        std::string usr = "fn:" + qualify(trait_qualified, mn.name);
        mn.metadata["usr"] = usr;
        mn.metadata["parameters"] = collect_parameters(field(member, "parameters"), source);
        ctx.fragment->nodes.push_back(mn);

        Edge e;
        e.id = ctx.local_ids->next();
        e.type = "contains";
        e.source = trait_id;
        e.target = mn.id;
        ctx.fragment->edges.push_back(e);

        if (kind == "function_item") {
            bool self_recv = has_self_receiver(field(member, "parameters"));
            walk_body(field(member, "body"), Mode::Read, usr, self_recv ? trait_qualified : "", source,
                      ctx.pending_calls, ctx.pending_reads, ctx.pending_writes);
        }
    }
}

void handle_free_function(TSNode node, VisitContext& ctx) {
    const std::string& source = *ctx.source;
    Node n;
    n.id = ctx.local_ids->next();
    n.type = "function";
    n.name = node_text(field(node, "name"), source);
    n.file = ctx.input->relative_path;
    n.language = "Rust";
    fill_line_range(node, n);
    std::string usr = "fn:" + qualify(ctx.qualifier, n.name);
    n.metadata["usr"] = usr;
    n.metadata["parameters"] = collect_parameters(field(node, "parameters"), source);
    TSNode ret = field(node, "return_type");
    if (!is_null(ret)) {
        n.metadata["return_type"] = node_text(ret, source);
    }
    ctx.fragment->nodes.push_back(n);

    Edge def;
    def.id = ctx.local_ids->next();
    def.type = "defines";
    def.source = ctx.container_node_id;
    def.target = n.id;
    ctx.fragment->edges.push_back(def);

    walk_body(field(node, "body"), Mode::Read, usr, "", source, ctx.pending_calls, ctx.pending_reads,
              ctx.pending_writes);
}

void add_mod_file_candidates(const std::string& mod_name, VisitContext& ctx) {
    fs::path current(ctx.input->absolute_path);
    fs::path dir = current.parent_path();
    std::string stem = current.stem().string();

    std::vector<fs::path> candidates = {dir / (mod_name + ".rs"), dir / mod_name / "mod.rs"};
    if (stem != "lib" && stem != "main" && stem != "mod") {
        candidates.push_back(dir / stem / (mod_name + ".rs"));
        candidates.push_back(dir / stem / mod_name / "mod.rs");
    }

    for (const fs::path& c : candidates) {
        std::error_code ec;
        fs::path canon = fs::weakly_canonical(c, ec);
        if (!ec) {
            ctx.pending_imports->push_back(PendingImport{ctx.input->file_node_id, canon.string()});
        }
    }
}

void visit_item(TSNode node, VisitContext& ctx);

void visit_children_as_items(TSNode parent, VisitContext& ctx) {
    uint32_t n = ts_node_named_child_count(parent);
    for (uint32_t i = 0; i < n; i++) {
        visit_item(ts_node_named_child(parent, i), ctx);
    }
}

void visit_item(TSNode node, VisitContext& ctx) {
    std::string kind = node_type(node);

    if (kind == "mod_item") {
        const std::string& source = *ctx.source;
        std::string mod_name = node_text(field(node, "name"), source);
        TSNode body = field(node, "body");
        if (is_null(body)) {
            add_mod_file_candidates(mod_name, ctx);
            return;
        }

        Node ns;
        ns.id = ctx.local_ids->next();
        ns.type = "namespace";
        ns.name = mod_name;
        ns.file = ctx.input->relative_path;
        ns.language = "Rust";
        fill_line_range(node, ns);
        ctx.fragment->nodes.push_back(ns);

        Edge def;
        def.id = ctx.local_ids->next();
        def.type = "defines";
        def.source = ctx.container_node_id;
        def.target = ns.id;
        ctx.fragment->edges.push_back(def);

        VisitContext nested = ctx;
        nested.container_node_id = ns.id;
        nested.qualifier = qualify(ctx.qualifier, mod_name);
        visit_children_as_items(body, nested);
        return;
    }

    if (kind == "struct_item") {
        handle_struct(node, ctx);
    } else if (kind == "enum_item") {
        handle_enum(node, ctx);
    } else if (kind == "trait_item") {
        handle_trait(node, ctx);
    } else if (kind == "impl_item") {
        handle_impl(node, ctx);
    } else if (kind == "function_item") {
        handle_free_function(node, ctx);
    }
}

}  

ParseResult RustParser::parseFile(const ParseInput& input) const {
    ParseResult result;

    std::ifstream file(input.absolute_path, std::ios::binary);
    if (!file) {
        return result;
    }
    std::ostringstream buf;
    buf << file.rdbuf();
    std::string source = buf.str();

    ParserHandle parser(tree_sitter_rust());
    TreeHandle tree(ts_parser_parse_string(parser.parser, nullptr, source.c_str(),
                                            static_cast<uint32_t>(source.size())));
    if (!tree.tree) {
        return result;
    }

    IdGenerator local_ids;
    VisitContext ctx{&result.fragment,
                      &local_ids,
                      &input,
                      &source,
                      input.file_node_id,
                      "",
                      &result.pending_calls,
                      &result.pending_reads,
                      &result.pending_writes,
                      &result.pending_contains,
                      &result.pending_imports};

    TSNode root = ts_tree_root_node(tree.tree);
    visit_children_as_items(root, ctx);

    return result;
}

} 
