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
};

// контекст, который передаётся в clang_visitChildren при обходе членов класса/структуры/юнита/энума/проче залупы
struct MemberVisitContext {
    VisitContext* ctx;
    std::string parent_node_id;
};

CXChildVisitResult visit_member_decl(CXCursor cursor, CXCursor /*parent*/, CXClientData data) {
    auto* mctx = static_cast<MemberVisitContext*>(data);
    CXCursorKind kind = clang_getCursorKind(cursor);

    if (kind != CXCursor_FieldDecl && kind != CXCursor_EnumConstantDecl) {
        return CXChildVisit_Continue;
    }

    Node node;
    node.id = mctx->ctx->local_ids->next();
    node.type = (kind == CXCursor_FieldDecl) ? "field" : "constant";
    node.name = to_std_string(clang_getCursorSpelling(cursor));
    node.file = mctx->ctx->input->relative_path;
    node.language = *mctx->ctx->language_label;
    fill_line_range(cursor, node);

    if (kind == CXCursor_FieldDecl) {
        CXType field_type = clang_getCursorType(cursor);
        node.metadata["field_type"] = to_std_string(clang_getTypeSpelling(field_type));
    } else {
        // Signed accessor only — a deliberate v0.2 simplification. Values
        // outside int64 range (or unsigned enums relying on the extra bit)
        // would need clang_getEnumConstantDeclUnsignedValue instead.
        long long value = clang_getEnumConstantDeclValue(cursor);
        node.metadata["value"] = value;
    }

    mctx->ctx->fragment->nodes.push_back(node);

    Edge contains;
    contains.id = mctx->ctx->local_ids->next();
    contains.type = "contains";
    contains.source = mctx->parent_node_id;
    contains.target = node.id;
    mctx->ctx->fragment->edges.push_back(contains);

    return CXChildVisit_Continue;
}

CXChildVisitResult visit_top_level_decl(CXCursor cursor, CXCursor /*parent*/, CXClientData data) {
    auto* ctx = static_cast<VisitContext*>(data);
    CXCursorKind kind = clang_getCursorKind(cursor);

    if (kind == CXCursor_Namespace || kind == CXCursor_LinkageSpec) {
        // Not a type declaration itself — just a scoping wrapper (namespace {}
        // / extern "C" {}). Recurse through it so structs/classes/unions/enums
        // declared directly inside are still found at "top level" of the
        // file, without extracting the namespace itself as a node yet, and
        // without recursing into anything ELSE nested (still no types nested
        // inside other types/functions).
        return CXChildVisit_Recurse;
    }
    if (kind != CXCursor_StructDecl && kind != CXCursor_ClassDecl &&
        kind != CXCursor_UnionDecl && kind != CXCursor_EnumDecl) {
        return CXChildVisit_Continue;
    }
    if (!clang_isCursorDefinition(cursor)) {
        return CXChildVisit_Continue;  // skip forward declarations
    }

    CXSourceLocation loc = clang_getCursorLocation(cursor);
    CXFile decl_file;
    unsigned line = 0, col = 0, offset = 0;
    clang_getExpansionLocation(loc, &decl_file, &line, &col, &offset);
    if (!clang_File_isEqual(decl_file, ctx->target_file)) {
        return CXChildVisit_Continue;  // declared via an #include'd header — skip
    }

    Node node;
    node.id = ctx->local_ids->next();
    node.type = kind == CXCursor_ClassDecl   ? "class"
                : kind == CXCursor_StructDecl ? "struct"
                : kind == CXCursor_UnionDecl  ? "union"
                                               : "enum";
    node.name = to_std_string(clang_getCursorSpelling(cursor));
    node.file = ctx->input->relative_path;
    node.language = *ctx->language_label;
    fill_line_range(cursor, node);

    ctx->fragment->nodes.push_back(node);

    Edge defines;
    defines.id = ctx->local_ids->next();
    defines.type = "defines";
    defines.source = ctx->input->file_node_id;
    defines.target = node.id;
    ctx->fragment->edges.push_back(defines);

    MemberVisitContext mctx{ctx, node.id};
    clang_visitChildren(cursor, &visit_member_decl, &mctx);

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

    VisitContext ctx{&fragment, &local_ids, &input, target_file, &language_label_};
    clang_visitChildren(root, &visit_top_level_decl, &ctx);

    return fragment;
}

}  
