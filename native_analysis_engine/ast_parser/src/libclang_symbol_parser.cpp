#include "libclang_symbol_parser.hpp"

#include <clang-c/Index.h>

#include <iostream>
#include <vector>

namespace codegraph {

namespace {

// это всё чтобы с памятью не заморачиваться и не забыть освободить ресурсы, которые libclang выделяет
struct ClangIndexHandle {
    CXIndex index;
    ClangIndexHandle() : index(clang_createIndex(/*excludeDeclFromPCH=*/0, /*displayDiagnostics=*/0)) {}
    ~ClangIndexHandle() { clang_disposeIndex(index); }
    ClangIndexHandle(const ClangIndexHandle&) = delete;
    ClangIndexHandle& operator=(const ClangIndexHandle&) = delete;
};

struct ClangTUHandle {
    CXTranslationUnit tu = nullptr;
    ~ClangTUHandle() {
        if (tu) {
            clang_disposeTranslationUnit(tu);
        }
    }
    ClangTUHandle(const ClangTUHandle&) = delete;
    ClangTUHandle& operator=(const ClangTUHandle&) = delete;
    ClangTUHandle() = default;
};

std::string to_std_string(CXString s) {
    const char* cstr = clang_getCString(s);
    std::string result = cstr ? cstr : "";
    clang_disposeString(s);
    return result;
}

// эта функция заполняет node.line_start и node.line_end по cursor, который указывает на declaration
void fill_line_range(CXCursor cursor, Node& node) {
    CXSourceRange extent = clang_getCursorExtent(cursor);

    CXFile f;
    unsigned line = 0, col = 0, offset = 0;
    clang_getExpansionLocation(clang_getRangeStart(extent), &f, &line, &col, &offset);
    node.line_start = static_cast<int>(line);
    clang_getExpansionLocation(clang_getRangeEnd(extent), &f, &line, &col, &offset);
    node.line_end = static_cast<int>(line);
}

// контекст, который передаётся в clang_visitChildren при обходе AST
struct VisitContext {
    Graph* fragment;
    IdGenerator* local_ids;
    const ParseInput* input;
    CXFile target_file;
    const std::string* language_label;
    // id узла File или Namespace, от которого должны идти "defines" edges для
    // top-level деклараций на текущем уровне вложенности. меняется только
    // через scoped-копию VisitContext при входе в namespace (visit_top_level_decl)
    // никогда не мутируется на месте, иначе сиблинги
    // после закрытия namespace ошибочно унаследуют его как контейнер
    std::string container_node_id;
};

// контекст, который передаётся в clang_visitChildren при обходе членов класса/структуры/юнита/энума/проче залупы
struct MemberVisitContext {
    VisitContext* ctx;
    std::string parent_node_id;
};

// контекст для сбора параметров функции/метода
struct ParamCollectContext {
    nlohmann::json* params;
};

// тут мы обходим ParmDecl-ов функции/метода и складываем их имя + тип в JSON-массив
CXChildVisitResult visit_param_decl(CXCursor cursor, CXCursor /* parent */, CXClientData data) {
    if (clang_getCursorKind(cursor) != CXCursor_ParmDecl) {
        return CXChildVisit_Continue;
    }
    auto* pctx = static_cast<ParamCollectContext*>(data);
    nlohmann::json param;
    param["name"] = to_std_string(clang_getCursorSpelling(cursor));
    param["type"] = to_std_string(clang_getTypeSpelling(clang_getCursorType(cursor)));
    pctx->params->push_back(std::move(param));
    return CXChildVisit_Continue;
}

nlohmann::json collect_parameters(CXCursor fn_cursor) {
    nlohmann::json params = nlohmann::json::array();
    ParamCollectContext pctx{&params};
    clang_visitChildren(fn_cursor, &visit_param_decl, &pctx);
    return params;
}

std::string access_specifier_to_string(CX_CXXAccessSpecifier access) {
    switch (access) {
        case CX_CXXPublic:
            return "public";
        case CX_CXXProtected:
            return "protected";
        case CX_CXXPrivate:
            return "private";
        default:
            return "";
    }
}

// тут мы обходим членов класса/структуры/юнита/энума (поля/константы/методы) и добавляем их в граф
CXChildVisitResult visit_member_decl(CXCursor cursor, CXCursor parent, CXClientData data) {
    auto* mctx = static_cast<MemberVisitContext*>(data);
    CXCursorKind kind = clang_getCursorKind(cursor);

    const bool is_method_like = kind == CXCursor_CXXMethod || kind == CXCursor_Constructor ||
                                 kind == CXCursor_Destructor;
    if (kind != CXCursor_FieldDecl && kind != CXCursor_EnumConstantDecl && !is_method_like) {
        return CXChildVisit_Continue;
    }

    if (is_method_like &&
        clang_equalLocations(clang_getCursorLocation(cursor), clang_getCursorLocation(parent))) {
       
        return CXChildVisit_Continue;
    }

    // пытаемся понять какой тип у поля/константы, чтобы положить его в metadata
    Node node;
    node.id = mctx->ctx->local_ids->next();
    node.type = (kind == CXCursor_FieldDecl)         ? "field"
                : (kind == CXCursor_EnumConstantDecl) ? "constant"
                                                       : "method";
    node.name = to_std_string(clang_getCursorSpelling(cursor));
    node.file = mctx->ctx->input->relative_path;
    node.language = *mctx->ctx->language_label;
    fill_line_range(cursor, node);

    // если это поле, то кладём его тип в metadata, если это константа, то кладём её значение в metadata
    if (kind == CXCursor_FieldDecl) {
        CXType field_type = clang_getCursorType(cursor);
        node.metadata["field_type"] = to_std_string(clang_getTypeSpelling(field_type));
    } else if (kind == CXCursor_EnumConstantDecl) {
        // clang_getEnumConstantDeclValue возвращает long long, потому что константа может быть отрицательной, мразь
        long long value = clang_getEnumConstantDeclValue(cursor);
        node.metadata["value"] = value;
    } else {
        // методы так идут -  parameters всегда, return_type только для обычных методов
        // (у ctor/dtor clang_getCursorResultType врёт "void", так что этот
        // ключ для них просто не пишем), visibility всегда (для метода это
        // всегда валидный access specifier, в отличие от свободных функций)
        node.metadata["parameters"] = collect_parameters(cursor);
        if (kind == CXCursor_CXXMethod) {
            node.metadata["return_type"] =
                to_std_string(clang_getTypeSpelling(clang_getCursorResultType(cursor)));
        }
        node.metadata["visibility"] = access_specifier_to_string(clang_getCXXAccessSpecifier(cursor));
    }

    mctx->ctx->fragment->nodes.push_back(node);

    // добавляем связь "contains" от родительского узла к этому узлу
    Edge contains;
    contains.id = mctx->ctx->local_ids->next();
    contains.type = "contains";
    contains.source = mctx->parent_node_id;
    contains.target = node.id;
    mctx->ctx->fragment->edges.push_back(contains);

    return CXChildVisit_Continue;
}

// тут мы обходим top-level declarations в файле (или внутри namespace) и добавляем их в граф
CXChildVisitResult visit_top_level_decl(CXCursor cursor, CXCursor /* parent */, CXClientData data) {
    auto* ctx = static_cast<VisitContext*>(data);
    CXCursorKind kind = clang_getCursorKind(cursor);

    if (kind == CXCursor_LinkageSpec) {
        
        return CXChildVisit_Recurse;
    }

    if (kind == CXCursor_Namespace) {
        // same-file фильтр применяем и к самому namespace тоже
        CXSourceLocation ns_loc = clang_getCursorLocation(cursor);
        CXFile ns_file;
        unsigned ns_line = 0, ns_col = 0, ns_offset = 0;
        clang_getExpansionLocation(ns_loc, &ns_file, &ns_line, &ns_col, &ns_offset);
        if (!clang_File_isEqual(ns_file, ctx->target_file)) {
            return CXChildVisit_Continue;
        }

        Node ns_node;
        ns_node.id = ctx->local_ids->next();
        ns_node.type = "namespace";
        ns_node.name = to_std_string(clang_getCursorSpelling(cursor));
        if (ns_node.name.empty()) {
            ns_node.name = "(anonymous namespace)";  // clang_getCursorSpelling пуст для namespace { ... }
        }
        ns_node.file = ctx->input->relative_path;
        ns_node.language = *ctx->language_label;
        fill_line_range(cursor, ns_node);
        ctx->fragment->nodes.push_back(ns_node);

        Edge ns_defines;
        ns_defines.id = ctx->local_ids->next();
        ns_defines.type = "defines";
        ns_defines.source = ctx->container_node_id;  // File либо охватывающий Namespace
        ns_defines.target = ns_node.id;
        ctx->fragment->edges.push_back(ns_defines);

        VisitContext nested_ctx = *ctx;
        nested_ctx.container_node_id = ns_node.id;
        clang_visitChildren(cursor, &visit_top_level_decl, &nested_ctx);
        return CXChildVisit_Continue;  // мы уже обошли детей вручную выше, повторно рекурсировать не надо
    }

    const bool is_type_decl = kind == CXCursor_StructDecl || kind == CXCursor_ClassDecl ||
                               kind == CXCursor_UnionDecl || kind == CXCursor_EnumDecl;
    if (!is_type_decl && kind != CXCursor_FunctionDecl) {
        return CXChildVisit_Continue;
    }
    
    if (is_type_decl && !clang_isCursorDefinition(cursor)) {
        return CXChildVisit_Continue;
    }

    // проверяем, что declaration находится в том же файле, что и target_file, иначе пропускаем
    CXSourceLocation loc = clang_getCursorLocation(cursor);
    CXFile decl_file;
    unsigned line = 0, col = 0, offset = 0;
    clang_getExpansionLocation(loc, &decl_file, &line, &col, &offset);
    if (!clang_File_isEqual(decl_file, ctx->target_file)) {
        return CXChildVisit_Continue;  // если declaration находится в другом файле, то пропускаем
    }

    // работаем с declaration, добавляем его в граф
    Node node;
    node.id = ctx->local_ids->next();
    node.type = kind == CXCursor_ClassDecl    ? "class"
                : kind == CXCursor_StructDecl ? "struct"
                : kind == CXCursor_UnionDecl  ? "union"
                : kind == CXCursor_EnumDecl   ? "enum"
                                               : "function";
    node.name = to_std_string(clang_getCursorSpelling(cursor));
    node.file = ctx->input->relative_path;
    node.language = *ctx->language_label;
    fill_line_range(cursor, node);

    if (kind == CXCursor_FunctionDecl) {
        node.metadata["parameters"] = collect_parameters(cursor);
        node.metadata["return_type"] =
            to_std_string(clang_getTypeSpelling(clang_getCursorResultType(cursor)));
    }

    ctx->fragment->nodes.push_back(node);

    // добавляем связь "defines" от текущего контейнера (файл либо namespace) к этому узлу
    Edge defines;
    defines.id = ctx->local_ids->next();
    defines.type = "defines";
    defines.source = ctx->container_node_id;
    defines.target = node.id;
    ctx->fragment->edges.push_back(defines);

    if (is_type_decl) {
        // тут мы обходим членов класса/структуры/юнита/энума и добавляем их в граф
        MemberVisitContext mctx{ctx, node.id};
        clang_visitChildren(cursor, &visit_member_decl, &mctx);
    }

    return CXChildVisit_Continue;
}

} 

LibclangSymbolParser::LibclangSymbolParser(std::string language_label, std::string clang_x_mode,
                                            std::string clang_std_flag)
    : language_label_(std::move(language_label)),
      clang_x_mode_(std::move(clang_x_mode)),
      clang_std_flag_(std::move(clang_std_flag)) {}

Graph LibclangSymbolParser::parseFile(const ParseInput& input) const {
    Graph fragment;
    IdGenerator local_ids;

    ClangIndexHandle index;
    ClangTUHandle tu_handle;

    std::vector<std::string> arg_storage = {"-x", clang_x_mode_, clang_std_flag_};
    std::vector<const char*> args;
    args.reserve(arg_storage.size());
    for (const auto& a : arg_storage) {
        args.push_back(a.c_str());
    }

    const unsigned options = CXTranslationUnit_SkipFunctionBodies | CXTranslationUnit_KeepGoing;

    CXErrorCode err = clang_parseTranslationUnit2(
        index.index, input.absolute_path.c_str(), args.data(), static_cast<int>(args.size()),
        /*unsaved_files=*/nullptr, /*num_unsaved_files=*/0, options, &tu_handle.tu);

    if (err != CXError_Success || tu_handle.tu == nullptr) {
        std::cerr << "warning: libclang could not parse " << input.relative_path
                   << " (CXErrorCode " << static_cast<int>(err)
                   << "), skipping symbol extraction for this file\n";
        return fragment;
    }

    CXFile target_file = clang_getFile(tu_handle.tu, input.absolute_path.c_str());
    CXCursor root = clang_getTranslationUnitCursor(tu_handle.tu);

    VisitContext ctx{&fragment, &local_ids, &input, target_file, &language_label_, input.file_node_id};
    clang_visitChildren(root, &visit_top_level_decl, &ctx);

    return fragment;
}

}  
