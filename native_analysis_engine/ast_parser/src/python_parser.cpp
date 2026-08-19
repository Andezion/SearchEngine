#include "python_parser.hpp"

#include <tree_sitter/api.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "tree_sitter_util.hpp"

extern "C" const TSLanguage* tree_sitter_python(void);

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
    std::string container_node_id;   // File либо class-узел (для вложенных классов)
    std::string container_edge_type;  // "defines" на уровне модуля, "contains" внутри класса
    std::string qualifier;            // путь вложенных классов для построения usr
    std::vector<PendingCall>* pending_calls;
    std::vector<PendingFieldAccess>* pending_reads;
    std::vector<PendingFieldAccess>* pending_writes;
    std::vector<PendingImport>* pending_imports;
};

void fill_line_range(TSNode node, Node& out) {
    out.line_start = line_start(node);
    out.line_end = line_end(node);
}

TSNode unwrap_decorated(TSNode node) {
    if (node_type(node) == "decorated_definition") {
        return field(node, "definition");
    }
    return node;
}

bool has_self_param(TSNode parameters, const std::string& source) {
    if (is_null(parameters) || ts_node_named_child_count(parameters) == 0) {
        return false;
    }
    TSNode first = ts_node_named_child(parameters, 0);
    std::string kind = node_type(first);
    if (kind == "identifier") {
        return node_text(first, source) == "self";
    }
    if (kind == "typed_parameter") {
        uint32_t m = ts_node_named_child_count(first);
        for (uint32_t i = 0; i < m; i++) {
            TSNode c = ts_node_named_child(first, i);
            if (node_type(c) == "identifier") {
                return node_text(c, source) == "self";
            }
        }
    }
    return false;
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
        std::string pname, ptype;
        if (kind == "identifier") {
            pname = node_text(p, source);
        } else if (kind == "typed_parameter" || kind == "typed_default_parameter") {
            TSNode type_n = field(p, "type");
            ptype = node_text(type_n, source);
            TSNode name_n = field(p, "name");
            if (!is_null(name_n)) {
                pname = node_text(name_n, source);
            } else {
                uint32_t m = ts_node_named_child_count(p);
                for (uint32_t j = 0; j < m; j++) {
                    TSNode c = ts_node_named_child(p, j);
                    if (node_type(c) == "identifier") {
                        pname = node_text(c, source);
                        break;
                    }
                }
            }
        } else if (kind == "default_parameter") {
            pname = node_text(field(p, "name"), source);
        } else {
            pname = node_text(p, source);
        }
        if (i == 0 && pname == "self") {
            continue;
        }
        nlohmann::json param;
        param["name"] = pname;
        param["type"] = ptype;
        params.push_back(std::move(param));
    }
    return params;
}

void walk_body(TSNode node, Mode mode, const std::string& owner_usr, const std::string& self_type_qualified,
               const std::string& source, std::vector<PendingCall>* calls,
               std::vector<PendingFieldAccess>* reads, std::vector<PendingFieldAccess>* writes) {
    if (is_null(node)) {
        return;
    }
    std::string kind = node_type(node);

    if (kind == "assignment" || kind == "augmented_assignment") {
        walk_body(field(node, "left"), Mode::Write, owner_usr, self_type_qualified, source, calls, reads, writes);
        TSNode right = field(node, "right");
        if (!is_null(right)) {
            walk_body(right, Mode::Read, owner_usr, self_type_qualified, source, calls, reads, writes);
        }
        return;
    }

    if (kind == "call") {
        TSNode fn = field(node, "function");
        std::string fn_kind = node_type(fn);
        std::optional<std::string> callee_usr;
        if (fn_kind == "identifier") {
            callee_usr = "fn:" + node_text(fn, source);
        } else if (fn_kind == "attribute") {
            TSNode obj = field(fn, "object");
            if (node_type(obj) == "identifier" && node_text(obj, source) == "self" && !self_type_qualified.empty()) {
                callee_usr = "fn:" + qualify(self_type_qualified, node_text(field(fn, "attribute"), source));
            }
        }
        if (callee_usr && !owner_usr.empty()) {
            calls->push_back(PendingCall{owner_usr, *callee_usr});
        }
        if (fn_kind == "attribute") {
            // .attribute тут - имя метода, а не поле данных - обходим
            // только объект, иначе он же засчитается как чтение поля
            walk_body(field(fn, "object"), Mode::Read, owner_usr, self_type_qualified, source, calls, reads,
                      writes);
        } else {
            walk_body(fn, Mode::Read, owner_usr, self_type_qualified, source, calls, reads, writes);
        }
        walk_body(field(node, "arguments"), Mode::Read, owner_usr, self_type_qualified, source, calls, reads,
                  writes);
        return;
    }

    if (kind == "attribute") {
        TSNode obj = field(node, "object");
        if (node_type(obj) == "identifier" && node_text(obj, source) == "self" && !self_type_qualified.empty()) {
            std::string field_usr = "field:" + qualify(self_type_qualified, node_text(field(node, "attribute"), source));
            if (mode == Mode::Write) {
                writes->push_back(PendingFieldAccess{owner_usr, field_usr});
            } else {
                reads->push_back(PendingFieldAccess{owner_usr, field_usr});
            }
            return;
        }
        walk_body(obj, Mode::Read, owner_usr, self_type_qualified, source, calls, reads, writes);
        return;
    }

    uint32_t n = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < n; i++) {
        walk_body(ts_node_named_child(node, i), mode, owner_usr, self_type_qualified, source, calls, reads, writes);
    }
}

void discover_self_fields(TSNode node, const std::string& source, std::vector<std::pair<std::string, TSNode>>& out,
                           std::set<std::string>& seen) {
    if (is_null(node)) {
        return;
    }
    std::string kind = node_type(node);
    if (kind == "assignment" || kind == "augmented_assignment") {
        TSNode left = field(node, "left");
        if (node_type(left) == "attribute") {
            TSNode obj = field(left, "object");
            if (node_type(obj) == "identifier" && node_text(obj, source) == "self") {
                std::string name = node_text(field(left, "attribute"), source);
                if (seen.insert(name).second) {
                    out.emplace_back(name, left);
                }
            }
        }
    }
    uint32_t n = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < n; i++) {
        discover_self_fields(ts_node_named_child(node, i), source, out, seen);
    }
}

std::string classify_python_class(TSNode node, const std::string& source) {
    TSNode supers = field(node, "superclasses");
    if (is_null(supers)) {
        return "class";
    }
    uint32_t n = ts_node_named_child_count(supers);
    for (uint32_t i = 0; i < n; i++) {
        TSNode base = ts_node_named_child(supers, i);
        std::string kind = node_type(base);
        std::string base_name;
        if (kind == "identifier") {
            base_name = node_text(base, source);
        } else if (kind == "attribute") {
            base_name = node_text(field(base, "attribute"), source);
        } else {
            continue;
        }
        if (base_name == "Enum" || base_name == "IntEnum" || base_name == "StrEnum" || base_name == "Flag" ||
            base_name == "IntFlag") {
            return "enum";
        }
        if (base_name == "Protocol" || base_name == "ABC") {
            return "interface";
        }
    }
    return "class";
}

void handle_class(TSNode node, VisitContext& ctx);

void add_method(TSNode fn_node, const std::string& class_id, const std::string& class_qualified,
                 VisitContext& ctx) {
    const std::string& source = *ctx.source;
    Node n;
    n.id = ctx.local_ids->next();
    n.type = "method";
    n.name = node_text(field(fn_node, "name"), source);
    n.file = ctx.input->relative_path;
    n.language = "Python";
    fill_line_range(fn_node, n);
    bool self_recv = has_self_param(field(fn_node, "parameters"), source);
    std::string usr = "fn:" + qualify(class_qualified, n.name);
    n.metadata["usr"] = usr;
    n.metadata["parameters"] = collect_parameters(field(fn_node, "parameters"), source);
    TSNode ret = field(fn_node, "return_type");
    if (!is_null(ret)) {
        n.metadata["return_type"] = node_text(ret, source);
    }
    ctx.fragment->nodes.push_back(n);

    Edge e;
    e.id = ctx.local_ids->next();
    e.type = "contains";
    e.source = class_id;
    e.target = n.id;
    ctx.fragment->edges.push_back(e);

    walk_body(field(fn_node, "body"), Mode::Read, usr, self_recv ? class_qualified : "", source, ctx.pending_calls,
              ctx.pending_reads, ctx.pending_writes);
}

void handle_class(TSNode node, VisitContext& ctx) {
    const std::string& source = *ctx.source;
    std::string name = node_text(field(node, "name"), source);
    std::string kind = classify_python_class(node, source);

    Node n;
    n.id = ctx.local_ids->next();
    n.type = kind;
    n.name = name;
    n.file = ctx.input->relative_path;
    n.language = "Python";
    fill_line_range(node, n);
    std::string type_qualified = qualify(ctx.qualifier, name);
    n.metadata["usr"] = "type:" + type_qualified;
    ctx.fragment->nodes.push_back(n);
    std::string class_id = n.id;

    Edge def;
    def.id = ctx.local_ids->next();
    def.type = ctx.container_edge_type;
    def.source = ctx.container_node_id;
    def.target = class_id;
    ctx.fragment->edges.push_back(def);

    TSNode body = field(node, "body");
    if (is_null(body)) {
        return;
    }

    std::vector<std::pair<std::string, TSNode>> self_fields;
    std::set<std::string> seen_fields;
    discover_self_fields(body, source, self_fields, seen_fields);
    for (const auto& [fname, fnode] : self_fields) {
        Node fld;
        fld.id = ctx.local_ids->next();
        fld.type = "field";
        fld.name = fname;
        fld.file = ctx.input->relative_path;
        fld.language = "Python";
        fill_line_range(fnode, fld);
        fld.metadata["usr"] = "field:" + qualify(type_qualified, fname);
        ctx.fragment->nodes.push_back(fld);

        Edge e;
        e.id = ctx.local_ids->next();
        e.type = "contains";
        e.source = class_id;
        e.target = fld.id;
        ctx.fragment->edges.push_back(e);
    }

    uint32_t cnt = ts_node_named_child_count(body);
    for (uint32_t i = 0; i < cnt; i++) {
        TSNode stmt = ts_node_named_child(body, i);
        TSNode inner = unwrap_decorated(stmt);
        std::string inner_kind = node_type(inner);

        if (inner_kind == "function_definition") {
            add_method(inner, class_id, type_qualified, ctx);
            continue;
        }
        if (inner_kind == "class_definition") {
            VisitContext nested = ctx;
            nested.container_node_id = class_id;
            nested.container_edge_type = "contains";
            nested.qualifier = type_qualified;
            handle_class(inner, nested);
            continue;
        }
        if (node_type(stmt) == "expression_statement" && ts_node_named_child_count(stmt) > 0) {
            TSNode expr = ts_node_named_child(stmt, 0);
            if (node_type(expr) == "assignment") {
                TSNode left = field(expr, "left");
                if (node_type(left) == "identifier") {
                    std::string cname = node_text(left, source);
                    Node cn;
                    cn.id = ctx.local_ids->next();
                    cn.type = (kind == "enum") ? "constant" : "field";
                    cn.name = cname;
                    cn.file = ctx.input->relative_path;
                    cn.language = "Python";
                    fill_line_range(expr, cn);
                    TSNode value = field(expr, "right");
                    if (!is_null(value)) {
                        cn.metadata["value"] = node_text(value, source);
                    }
                    if (cn.type == "field") {
                        cn.metadata["usr"] = "field:" + qualify(type_qualified, cname);
                    }
                    ctx.fragment->nodes.push_back(cn);

                    Edge e;
                    e.id = ctx.local_ids->next();
                    e.type = "contains";
                    e.source = class_id;
                    e.target = cn.id;
                    ctx.fragment->edges.push_back(e);
                }
            }
        }
    }
}

void handle_function(TSNode node, VisitContext& ctx) {
    const std::string& source = *ctx.source;
    Node n;
    n.id = ctx.local_ids->next();
    n.type = "function";
    n.name = node_text(field(node, "name"), source);
    n.file = ctx.input->relative_path;
    n.language = "Python";
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
    def.type = ctx.container_edge_type;
    def.source = ctx.container_node_id;
    def.target = n.id;
    ctx.fragment->edges.push_back(def);

    walk_body(field(node, "body"), Mode::Read, usr, "", source, ctx.pending_calls, ctx.pending_reads,
              ctx.pending_writes);
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

void add_module_candidates(const fs::path& base_dir, const std::string& dotted_path, VisitContext& ctx) {
    if (dotted_path.empty()) {
        return;
    }
    std::string as_path;
    as_path.reserve(dotted_path.size());
    for (char c : dotted_path) {
        as_path += (c == '.') ? '/' : c;
    }
    std::vector<fs::path> candidates = {base_dir / (as_path + ".py"), base_dir / as_path / "__init__.py"};
    for (const fs::path& c : candidates) {
        std::error_code ec;
        fs::path canon = fs::weakly_canonical(c, ec);
        if (!ec) {
            ctx.pending_imports->push_back(PendingImport{ctx.input->file_node_id, canon.string()});
        }
    }
}

std::string dotted_text(TSNode name_node, const std::string& source) {
    TSNode dotted = name_node;
    if (node_type(dotted) == "aliased_import") {
        dotted = field(dotted, "name");
    }
    if (node_type(dotted) != "dotted_name") {
        return "";
    }
    return node_text(dotted, source);
}

void handle_import_statement(TSNode node, VisitContext& ctx) {
    const std::string& source = *ctx.source;
    fs::path project_root = compute_project_root(*ctx.input);
    fs::path current_dir = fs::path(ctx.input->absolute_path).parent_path();
    for (TSNode name_node : multi_field(node, "name")) {
        std::string path_text = dotted_text(name_node, source);
        if (path_text.empty()) {
            continue;
        }
        add_module_candidates(project_root, path_text, ctx);
        add_module_candidates(current_dir, path_text, ctx);
    }
}

void handle_import_from_statement(TSNode node, VisitContext& ctx) {
    const std::string& source = *ctx.source;
    fs::path project_root = compute_project_root(*ctx.input);
    fs::path current_dir = fs::path(ctx.input->absolute_path).parent_path();

    TSNode module_name = field(node, "module_name");
    std::string mkind = node_type(module_name);
    std::vector<fs::path> base_dirs;

    if (mkind == "dotted_name") {
        std::string module_path_text = node_text(module_name, source);
        add_module_candidates(project_root, module_path_text, ctx);
        add_module_candidates(current_dir, module_path_text, ctx);
        std::string as_path;
        for (char c : module_path_text) {
            as_path += (c == '.') ? '/' : c;
        }
        base_dirs.push_back(project_root / as_path);
        base_dirs.push_back(current_dir / as_path);
    } else if (mkind == "relative_import") {
        int dot_count = 0;
        TSNode dotted_child{};
        bool has_dotted = false;
        uint32_t n = ts_node_named_child_count(module_name);
        for (uint32_t i = 0; i < n; i++) {
            TSNode c = ts_node_named_child(module_name, i);
            std::string k = node_type(c);
            if (k == "import_prefix") {
                dot_count = static_cast<int>(node_text(c, source).size());
            } else if (k == "dotted_name") {
                dotted_child = c;
                has_dotted = true;
            }
        }
        fs::path base = current_dir;
        for (int i = 1; i < dot_count; i++) {
            base = base.parent_path();
        }
        if (has_dotted) {
            std::string sub = node_text(dotted_child, source);
            add_module_candidates(base, sub, ctx);
            std::string as_path;
            for (char c : sub) {
                as_path += (c == '.') ? '/' : c;
            }
            base_dirs.push_back(base / as_path);
        } else {
            base_dirs.push_back(base);
        }
    }

    for (TSNode name_node : multi_field(node, "name")) {
        std::string sym = dotted_text(name_node, source);
        if (sym.empty()) {
            continue;
        }
        for (const fs::path& bd : base_dirs) {
            add_module_candidates(bd, sym, ctx);
        }
    }
}

}  

ParseResult PythonParser::parseFile(const ParseInput& input) const {
    ParseResult result;

    std::ifstream file(input.absolute_path, std::ios::binary);
    if (!file) {
        return result;
    }
    std::ostringstream buf;
    buf << file.rdbuf();
    std::string source = buf.str();

    ParserHandle parser(tree_sitter_python());
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
                      "defines",
                      "",
                      &result.pending_calls,
                      &result.pending_reads,
                      &result.pending_writes,
                      &result.pending_imports};

    TSNode root = ts_tree_root_node(tree.tree);
    uint32_t n = ts_node_named_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        TSNode stmt = ts_node_named_child(root, i);
        TSNode inner = unwrap_decorated(stmt);
        std::string kind = node_type(inner);
        if (kind == "class_definition") {
            handle_class(inner, ctx);
        } else if (kind == "function_definition") {
            handle_function(inner, ctx);
        } else if (node_type(stmt) == "import_statement") {
            handle_import_statement(stmt, ctx);
        } else if (node_type(stmt) == "import_from_statement") {
            handle_import_from_statement(stmt, ctx);
        }
    }

    return result;
}

}  
