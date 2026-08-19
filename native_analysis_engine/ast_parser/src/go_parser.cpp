#include "go_parser.hpp"

#include <tree_sitter/api.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "tree_sitter_util.hpp"

extern "C" const TSLanguage* tree_sitter_go(void);

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
    std::string container_node_id;  // File - у Go нет вложенных пространств имён, package не моделируем узлом
    std::string package_name;       // квалификатор для usr, не создаёт узла в графе
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
        if (node_type(p) != "parameter_declaration") {
            continue;
        }
        std::string type_text = node_text(field(p, "type"), source);
        for (TSNode name_node : multi_field(p, "name")) {
            nlohmann::json param;
            param["name"] = node_text(name_node, source);
            param["type"] = type_text;
            params.push_back(std::move(param));
        }
        if (multi_field(p, "name").empty()) {
            nlohmann::json param;
            param["name"] = "";
            param["type"] = type_text;
            params.push_back(std::move(param));
        }
    }
    return params;
}

// обходим тело функции/метода в поисках вызовов и чтений/записей
// receiver.field - synтaксический аналог visit_for_calls/visit_for_dataflow
void walk_body(TSNode node, Mode mode, const std::string& owner_usr, const std::string& pkg_qualifier,
               const std::string& receiver_var, const std::string& self_type_qualified, const std::string& source,
               std::vector<PendingCall>* calls, std::vector<PendingFieldAccess>* reads,
               std::vector<PendingFieldAccess>* writes) {
    if (is_null(node)) {
        return;
    }
    std::string kind = node_type(node);

    if (kind == "assignment_statement" || kind == "short_var_declaration") {
        walk_body(field(node, "left"), Mode::Write, owner_usr, pkg_qualifier, receiver_var, self_type_qualified,
                  source, calls, reads, writes);
        walk_body(field(node, "right"), Mode::Read, owner_usr, pkg_qualifier, receiver_var, self_type_qualified,
                  source, calls, reads, writes);
        return;
    }

    if (kind == "call_expression") {
        TSNode fn = field(node, "function");
        std::string fn_kind = node_type(fn);
        std::optional<std::string> callee_usr;
        if (fn_kind == "identifier") {
            callee_usr = "fn:" + qualify(pkg_qualifier, node_text(fn, source));
        } else if (fn_kind == "selector_expression") {
            TSNode operand = field(fn, "operand");
            if (node_type(operand) == "identifier" && node_text(operand, source) == receiver_var &&
                !self_type_qualified.empty()) {
                callee_usr = "fn:" + qualify(self_type_qualified, node_text(field(fn, "field"), source));
            }
        }
        if (callee_usr && !owner_usr.empty()) {
            calls->push_back(PendingCall{owner_usr, *callee_usr});
        }
        if (fn_kind == "selector_expression") {
            // .field тут - имя метода, а не поле данных - обходим только
            // operand, иначе он же попадёт как чтение поля с именем метода
            walk_body(field(fn, "operand"), Mode::Read, owner_usr, pkg_qualifier, receiver_var, self_type_qualified,
                      source, calls, reads, writes);
        } else {
            walk_body(fn, Mode::Read, owner_usr, pkg_qualifier, receiver_var, self_type_qualified, source, calls,
                      reads, writes);
        }
        walk_body(field(node, "arguments"), Mode::Read, owner_usr, pkg_qualifier, receiver_var, self_type_qualified,
                  source, calls, reads, writes);
        return;
    }

    if (kind == "selector_expression") {
        TSNode operand = field(node, "operand");
        if (node_type(operand) == "identifier" && node_text(operand, source) == receiver_var &&
            !self_type_qualified.empty()) {
            std::string field_usr = "field:" + qualify(self_type_qualified, node_text(field(node, "field"), source));
            if (mode == Mode::Write) {
                writes->push_back(PendingFieldAccess{owner_usr, field_usr});
            } else {
                reads->push_back(PendingFieldAccess{owner_usr, field_usr});
            }
            return;
        }
        walk_body(operand, Mode::Read, owner_usr, pkg_qualifier, receiver_var, self_type_qualified, source, calls,
                  reads, writes);
        return;
    }

    uint32_t n = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < n; i++) {
        walk_body(ts_node_named_child(node, i), mode, owner_usr, pkg_qualifier, receiver_var, self_type_qualified,
                  source, calls, reads, writes);
    }
}

void add_struct_field(TSNode field_decl, const std::string& parent_id, const std::string& type_qualified,
                       VisitContext& ctx) {
    const std::string& source = *ctx.source;
    std::string type_text = node_text(field(field_decl, "type"), source);
    std::vector<TSNode> names = multi_field(field_decl, "name");

    auto push_field = [&](const std::string& name) {
        Node n;
        n.id = ctx.local_ids->next();
        n.type = "field";
        n.name = name;
        n.file = ctx.input->relative_path;
        n.language = "Go";
        fill_line_range(field_decl, n);
        n.metadata["field_type"] = type_text;
        n.metadata["usr"] = "field:" + qualify(type_qualified, name);
        ctx.fragment->nodes.push_back(n);

        Edge e;
        e.id = ctx.local_ids->next();
        e.type = "contains";
        e.source = parent_id;
        e.target = n.id;
        ctx.fragment->edges.push_back(e);
    };

    if (names.empty()) {
        push_field(base_type_name(field(field_decl, "type"), source));
    } else {
        for (TSNode name_node : names) {
            push_field(node_text(name_node, source));
        }
    }
}

void handle_struct(TSNode name_node, TSNode struct_type, VisitContext& ctx) {
    const std::string& source = *ctx.source;
    std::string name = node_text(name_node, source);

    Node n;
    n.id = ctx.local_ids->next();
    n.type = "struct";
    n.name = name;
    n.file = ctx.input->relative_path;
    n.language = "Go";
    fill_line_range(struct_type, n);
    std::string type_qualified = qualify(ctx.package_name, name);
    n.metadata["usr"] = "type:" + type_qualified;
    ctx.fragment->nodes.push_back(n);
    std::string struct_id = n.id;

    Edge def;
    def.id = ctx.local_ids->next();
    def.type = "defines";
    def.source = ctx.container_node_id;
    def.target = struct_id;
    ctx.fragment->edges.push_back(def);

    TSNode fdl = ts_node_named_child_count(struct_type) > 0 ? ts_node_named_child(struct_type, 0) : TSNode{};
    if (!is_null(fdl) && node_type(fdl) == "field_declaration_list") {
        uint32_t cnt = ts_node_named_child_count(fdl);
        for (uint32_t i = 0; i < cnt; i++) {
            TSNode f = ts_node_named_child(fdl, i);
            if (node_type(f) == "field_declaration") {
                add_struct_field(f, struct_id, type_qualified, ctx);
            }
        }
    }
}

void handle_interface(TSNode name_node, TSNode interface_type, VisitContext& ctx) {
    const std::string& source = *ctx.source;
    std::string name = node_text(name_node, source);

    Node n;
    n.id = ctx.local_ids->next();
    n.type = "interface";
    n.name = name;
    n.file = ctx.input->relative_path;
    n.language = "Go";
    fill_line_range(interface_type, n);
    std::string type_qualified = qualify(ctx.package_name, name);
    n.metadata["usr"] = "type:" + type_qualified;
    ctx.fragment->nodes.push_back(n);
    std::string iface_id = n.id;

    Edge def;
    def.id = ctx.local_ids->next();
    def.type = "defines";
    def.source = ctx.container_node_id;
    def.target = iface_id;
    ctx.fragment->edges.push_back(def);

    uint32_t cnt = ts_node_named_child_count(interface_type);
    for (uint32_t i = 0; i < cnt; i++) {
        TSNode member = ts_node_named_child(interface_type, i);
        if (node_type(member) != "method_elem") {
            continue;
        }
        Node mn;
        mn.id = ctx.local_ids->next();
        mn.type = "method";
        mn.name = node_text(field(member, "name"), source);
        mn.file = ctx.input->relative_path;
        mn.language = "Go";
        fill_line_range(member, mn);
        mn.metadata["usr"] = "fn:" + qualify(type_qualified, mn.name);
        mn.metadata["parameters"] = collect_parameters(field(member, "parameters"), source);
        ctx.fragment->nodes.push_back(mn);

        Edge e;
        e.id = ctx.local_ids->next();
        e.type = "contains";
        e.source = iface_id;
        e.target = mn.id;
        ctx.fragment->edges.push_back(e);
    }
}

void handle_type_declaration(TSNode node, VisitContext& ctx) {
    uint32_t n = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < n; i++) {
        TSNode spec = ts_node_named_child(node, i);
        if (node_type(spec) != "type_spec") {
            continue;
        }
        TSNode name_node = field(spec, "name");
        TSNode type_node = field(spec, "type");
        std::string type_kind = node_type(type_node);
        if (type_kind == "struct_type") {
            handle_struct(name_node, type_node, ctx);
        } else if (type_kind == "interface_type") {
            handle_interface(name_node, type_node, ctx);
        }
        // алиасы на примитивы/другие типы вне зоны графа - пропускаем
    }
}

bool spec_uses_iota(TSNode spec, const std::string& source) {
    TSNode value = field(spec, "value");
    if (is_null(value)) {
        return false;
    }
    return node_text(value, source).find("iota") != std::string::npos;
}

void handle_const_declaration(TSNode node, VisitContext& ctx) {
    const std::string& source = *ctx.source;
    uint32_t n = ts_node_named_child_count(node);

    bool is_enum_like = false;
    for (uint32_t i = 0; i < n; i++) {
        TSNode spec = ts_node_named_child(node, i);
        if (node_type(spec) == "const_spec" && spec_uses_iota(spec, source)) {
            is_enum_like = true;
            break;
        }
    }
    if (!is_enum_like) {
        return;  // обычные константы вне зоны графа - как и свободные переменные в C++
    }

    std::string enum_name = "constants";
    for (uint32_t i = 0; i < n; i++) {
        TSNode spec = ts_node_named_child(node, i);
        if (node_type(spec) != "const_spec") {
            continue;
        }
        TSNode type_node = field(spec, "type");
        if (!is_null(type_node)) {
            enum_name = node_text(type_node, source);
            break;
        }
    }

    Node en;
    en.id = ctx.local_ids->next();
    en.type = "enum";
    en.name = enum_name;
    en.file = ctx.input->relative_path;
    en.language = "Go";
    fill_line_range(node, en);
    en.metadata["usr"] = "type:" + qualify(ctx.package_name, enum_name + "@" + std::to_string(line_start(node)));
    ctx.fragment->nodes.push_back(en);
    std::string enum_id = en.id;

    Edge def;
    def.id = ctx.local_ids->next();
    def.type = "defines";
    def.source = ctx.container_node_id;
    def.target = enum_id;
    ctx.fragment->edges.push_back(def);

    for (uint32_t i = 0; i < n; i++) {
        TSNode spec = ts_node_named_child(node, i);
        if (node_type(spec) != "const_spec") {
            continue;
        }
        for (TSNode name_node : multi_field(spec, "name")) {
            Node cn;
            cn.id = ctx.local_ids->next();
            cn.type = "constant";
            cn.name = node_text(name_node, source);
            cn.file = ctx.input->relative_path;
            cn.language = "Go";
            fill_line_range(spec, cn);
            TSNode value = field(spec, "value");
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
}

void handle_function_declaration(TSNode node, VisitContext& ctx) {
    const std::string& source = *ctx.source;
    Node n;
    n.id = ctx.local_ids->next();
    n.type = "function";
    n.name = node_text(field(node, "name"), source);
    n.file = ctx.input->relative_path;
    n.language = "Go";
    fill_line_range(node, n);
    std::string usr = "fn:" + qualify(ctx.package_name, n.name);
    n.metadata["usr"] = usr;
    n.metadata["parameters"] = collect_parameters(field(node, "parameters"), source);
    ctx.fragment->nodes.push_back(n);

    Edge def;
    def.id = ctx.local_ids->next();
    def.type = "defines";
    def.source = ctx.container_node_id;
    def.target = n.id;
    ctx.fragment->edges.push_back(def);

    walk_body(field(node, "body"), Mode::Read, usr, ctx.package_name, "", "", source, ctx.pending_calls,
              ctx.pending_reads, ctx.pending_writes);
}

void handle_method_declaration(TSNode node, VisitContext& ctx) {
    const std::string& source = *ctx.source;
    TSNode receiver_list = field(node, "receiver");
    if (is_null(receiver_list) || ts_node_named_child_count(receiver_list) == 0) {
        return;
    }
    TSNode receiver = ts_node_named_child(receiver_list, 0);
    std::vector<TSNode> recv_names = multi_field(receiver, "name");
    std::string receiver_var = recv_names.empty() ? "" : node_text(recv_names[0], source);
    std::string receiver_type = base_type_name(field(receiver, "type"), source);
    if (receiver_type.empty()) {
        return;
    }
    std::string self_type_qualified = qualify(ctx.package_name, receiver_type);

    Node n;
    n.id = ctx.local_ids->next();
    n.type = "method";
    n.name = node_text(field(node, "name"), source);
    n.file = ctx.input->relative_path;
    n.language = "Go";
    fill_line_range(node, n);
    std::string usr = "fn:" + qualify(self_type_qualified, n.name);
    n.metadata["usr"] = usr;
    n.metadata["parameters"] = collect_parameters(field(node, "parameters"), source);
    ctx.fragment->nodes.push_back(n);

    Edge def;
    def.id = ctx.local_ids->next();
    def.type = "defines";
    def.source = ctx.container_node_id;
    def.target = n.id;
    ctx.fragment->edges.push_back(def);

    ctx.pending_contains->push_back(PendingContains{"type:" + self_type_qualified, usr});

    walk_body(field(node, "body"), Mode::Read, usr, ctx.package_name, receiver_var, self_type_qualified, source,
              ctx.pending_calls, ctx.pending_reads, ctx.pending_writes);
}

fs::path compute_project_root(const ParseInput& input) {
    fs::path abs(input.absolute_path);
    fs::path rel(input.relative_path);
    fs::path root = abs;
    long depth = std::distance(rel.begin(), rel.end());
    for (long i = 0; i < depth; i++) {
        root = root.parent_path();
    }
    return root;
}

std::optional<std::pair<fs::path, std::string>> find_go_mod(fs::path dir, const fs::path& project_root) {
    while (true) {
        std::error_code ec;
        fs::path candidate = dir / "go.mod";
        if (fs::exists(candidate, ec)) {
            std::ifstream f(candidate);
            std::string line;
            while (std::getline(f, line)) {
                if (line.rfind("module ", 0) == 0) {
                    std::string mod = line.substr(7);
                    while (!mod.empty() && (mod.back() == ' ' || mod.back() == '\t' || mod.back() == '\r')) {
                        mod.pop_back();
                    }
                    return std::make_pair(dir, mod);
                }
            }
            return std::nullopt;
        }
        if (dir == project_root) {
            break;
        }
        fs::path parent = dir.parent_path();
        if (parent == dir) {
            break;
        }
        dir = parent;
    }
    return std::nullopt;
}

void resolve_go_import(const std::string& import_path, VisitContext& ctx) {
    fs::path current(ctx.input->absolute_path);
    fs::path project_root = compute_project_root(*ctx.input);
    auto modinfo = find_go_mod(current.parent_path(), project_root);
    if (!modinfo) {
        return;
    }
    const fs::path& mod_root = modinfo->first;
    const std::string& mod_path = modinfo->second;

    fs::path target;
    if (import_path == mod_path) {
        target = mod_root;
    } else if (import_path.rfind(mod_path + "/", 0) == 0) {
        std::string rel = import_path.substr(mod_path.size() + 1);
        target = mod_root / fs::path(rel);
    } else {
        return;  // внешняя зависимость (stdlib/третья сторона) - не в этом проекте
    }

    std::error_code ec;
    fs::path canon = fs::weakly_canonical(target, ec);
    if (!ec) {
        ctx.pending_imports->push_back(PendingImport{ctx.input->file_node_id, canon.string()});
    }
}

void collect_import_specs(TSNode node, std::vector<TSNode>& out) {
    if (node_type(node) == "import_spec") {
        out.push_back(node);
        return;
    }
    uint32_t n = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < n; i++) {
        collect_import_specs(ts_node_named_child(node, i), out);
    }
}

void handle_import_declaration(TSNode node, VisitContext& ctx) {
    const std::string& source = *ctx.source;
    std::vector<TSNode> specs;
    collect_import_specs(node, specs);
    for (TSNode spec : specs) {
        TSNode path_node = field(spec, "path");
        std::string raw = node_text(path_node, source);
        if (raw.size() >= 2) {
            raw = raw.substr(1, raw.size() - 2);  // снимаем кавычки строкового литерала
        }
        resolve_go_import(raw, ctx);
    }
}

}  

ParseResult GoParser::parseFile(const ParseInput& input) const {
    ParseResult result;

    std::ifstream file(input.absolute_path, std::ios::binary);
    if (!file) {
        return result;
    }
    std::ostringstream buf;
    buf << file.rdbuf();
    std::string source = buf.str();

    ParserHandle parser(tree_sitter_go());
    TreeHandle tree(ts_parser_parse_string(parser.parser, nullptr, source.c_str(),
                                            static_cast<uint32_t>(source.size())));
    if (!tree.tree) {
        return result;
    }

    IdGenerator local_ids;
    TSNode root = ts_tree_root_node(tree.tree);

    std::string package_name;
    uint32_t root_n = ts_node_named_child_count(root);
    for (uint32_t i = 0; i < root_n; i++) {
        TSNode top = ts_node_named_child(root, i);
        if (node_type(top) == "package_clause" && ts_node_named_child_count(top) > 0) {
            package_name = node_text(ts_node_named_child(top, 0), source);
            break;
        }
    }

    VisitContext ctx{&result.fragment,
                      &local_ids,
                      &input,
                      &source,
                      input.file_node_id,
                      package_name,
                      &result.pending_calls,
                      &result.pending_reads,
                      &result.pending_writes,
                      &result.pending_contains,
                      &result.pending_imports};

    for (uint32_t i = 0; i < root_n; i++) {
        TSNode top = ts_node_named_child(root, i);
        std::string kind = node_type(top);
        if (kind == "function_declaration") {
            handle_function_declaration(top, ctx);
        } else if (kind == "method_declaration") {
            handle_method_declaration(top, ctx);
        } else if (kind == "type_declaration") {
            handle_type_declaration(top, ctx);
        } else if (kind == "const_declaration") {
            handle_const_declaration(top, ctx);
        } else if (kind == "import_declaration") {
            handle_import_declaration(top, ctx);
        }
    }

    return result;
}

}  
