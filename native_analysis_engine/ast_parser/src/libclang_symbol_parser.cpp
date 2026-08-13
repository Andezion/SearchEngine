#include "libclang_symbol_parser.hpp"

#include <clang-c/Index.h>

#include <filesystem>
#include <iostream>
#include <set>
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

std::string classify_type_kind(CXType type) {
    CXType canonical = clang_getCanonicalType(type);

    for (;;) {
        CXTypeKind k = canonical.kind;
        if (k == CXType_Pointer || k == CXType_LValueReference || k == CXType_RValueReference) {
            CXType pointee = clang_getCanonicalType(clang_getPointeeType(canonical));
            if (pointee.kind == CXType_FunctionProto || pointee.kind == CXType_FunctionNoProto) {
                return "function";  
            }
            canonical = pointee;
            continue;
        }
        if (k == CXType_ConstantArray || k == CXType_IncompleteArray || k == CXType_VariableArray ||
            k == CXType_DependentSizedArray) {
            canonical = clang_getCanonicalType(clang_getArrayElementType(canonical));
            continue;
        }
        break;
    }

    switch (canonical.kind) {
        case CXType_FunctionProto:
        case CXType_FunctionNoProto:
            return "function";
        case CXType_Enum: {
            CXCursor decl = clang_getTypeDeclaration(canonical);
            return clang_EnumDecl_isScoped(decl) ? "enum_class" : "enum";
        }
        case CXType_Record: {
            CXCursor decl = clang_getTypeDeclaration(canonical);
            switch (clang_getCursorKind(decl)) {
                case CXCursor_StructDecl:
                    return "struct";
                case CXCursor_UnionDecl:
                    return "union";
                default:
                    return "class";  // ClassDecl и шаблонные специализацииб типа std::vector<T> и так далее - тоже "class"
            }
        }
        default:
            break;
    }

    if (canonical.kind >= CXType_FirstBuiltin && canonical.kind <= CXType_LastBuiltin) {
        return "value";  
    }

    return "unknown";  
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
    std::vector<PendingCall>* pending_calls;
    std::vector<PendingFieldAccess>* pending_reads;
    std::vector<PendingFieldAccess>* pending_writes;
};

// USR (Unified Symbol Resolution) вызывающей функции/метода + куда собирать
// найденные вызовы, пока обходим её тело
struct CallCollectContext {
    std::string caller_usr;
    std::vector<PendingCall>* pending_calls;
};


CXChildVisitResult visit_for_calls(CXCursor cursor, CXCursor /* parent */, CXClientData data) {
    auto* cctx = static_cast<CallCollectContext*>(data);
    if (clang_getCursorKind(cursor) == CXCursor_CallExpr) {
        CXCursor callee = clang_getCursorReferenced(cursor);
        if (!clang_Cursor_isNull(callee)) {
            std::string callee_usr = to_std_string(clang_getCursorUSR(callee));
            if (!callee_usr.empty()) {
                cctx->pending_calls->push_back(PendingCall{cctx->caller_usr, callee_usr});
            }
        }
    }
    return CXChildVisit_Recurse;
}

enum class AccessMode { Read, Write };

// USR того, кто обращается к полю (функция/метод) + текущий режим доступа +
// куда собирать найденные чтения/записи, пока обходим тело
struct DataFlowContext {
    std::string owner_usr;
    AccessMode mode;
    std::vector<PendingFieldAccess>* pending_reads;
    std::vector<PendingFieldAccess>* pending_writes;
};

CXChildVisitResult visit_for_dataflow(CXCursor cursor, CXCursor parent, CXClientData data);

struct ChildModeDispatch {
    DataFlowContext* dctx;
    int index = 0;
};

CXChildVisitResult dispatch_child_mode(CXCursor child, CXCursor parent, CXClientData data) {
    auto* d = static_cast<ChildModeDispatch*>(data);
    DataFlowContext child_ctx = *d->dctx;
    child_ctx.mode = (d->index == 0) ? AccessMode::Write : AccessMode::Read;
    d->index++;
    visit_for_dataflow(child, parent, &child_ctx);
    return CXChildVisit_Continue;
}

// Обходим тело функции/метода в поисках чтений/записей полей. Никогда не
// возвращает Recurse - вместо этого сами решаем, с каким режимом (Read или
// Write) идти в детей, и вызываем visit_for_dataflow напрямую
CXChildVisitResult visit_for_dataflow(CXCursor cursor, CXCursor /* parent */, CXClientData data) {
    auto* dctx = static_cast<DataFlowContext*>(data);
    CXCursorKind kind = clang_getCursorKind(cursor);

    bool is_plain_assign = kind == CXCursor_BinaryOperator &&
                            clang_getCursorBinaryOperatorKind(cursor) == CXBinaryOperator_Assign;
    bool is_compound_assign = kind == CXCursor_CompoundAssignOperator;  // любой opcode - запись
    bool is_incdec = false;
    if (kind == CXCursor_UnaryOperator) {
        enum CXUnaryOperatorKind uop = clang_getCursorUnaryOperatorKind(cursor);
        is_incdec = uop == CXUnaryOperator_PostInc || uop == CXUnaryOperator_PostDec ||
                    uop == CXUnaryOperator_PreInc || uop == CXUnaryOperator_PreDec;
    }

    if (is_plain_assign || is_compound_assign || is_incdec) {
        ChildModeDispatch disp{dctx, 0};
        clang_visitChildren(cursor, &dispatch_child_mode, &disp);
        return CXChildVisit_Continue;
    }

    if (kind == CXCursor_MemberRefExpr) {
        CXCursor referenced = clang_getCursorReferenced(cursor);
        // MemberRefExpr резолвится и в поля, и в методы (obj.method()
        // тоже даёт MemberRefExpr) - без этой проверки каждый вызов метода
        // превращался бы в ложное "чтение" вызываемого метода как будто это поле
        if (!clang_Cursor_isNull(referenced) && clang_getCursorKind(referenced) == CXCursor_FieldDecl) {
            std::string field_usr = to_std_string(clang_getCursorUSR(referenced));
            if (!field_usr.empty()) {
                PendingFieldAccess pfa{dctx->owner_usr, field_usr};
                if (dctx->mode == AccessMode::Write) {
                    dctx->pending_writes->push_back(pfa);
                } else {
                    dctx->pending_reads->push_back(pfa);
                }
            }
        }
        clang_visitChildren(cursor, &visit_for_dataflow, dctx);  // тот же режим (может быть цепочка a b c)
        return CXChildVisit_Continue;
    }

    clang_visitChildren(cursor, &visit_for_dataflow, dctx);  // режим не меняется
    return CXChildVisit_Continue;
}

struct InclusionCollectContext {
    CXFile target_file;
    std::set<std::string>* direct_include_paths;
};

// clang_getInclusions зовёт это для КАЖДОГО #include во всём TU, включая
// транзитивные. include_len==1 означает, что вся цепочка включения - это
// ровно "включён напрямую из файла, который мы сейчас парсим" (стек из
// одной локации), то есть прямой #include этого файла, а не что-то
// затянутое через другой заголовок - те корректно всплывут сами, когда
// промежуточный заголовок будет распарсен как свой собственный target_file
void visit_inclusion(CXFile included_file, CXSourceLocation* /* inclusion_stack */,
                      unsigned include_len, CXClientData data) {
    auto* ictx = static_cast<InclusionCollectContext*>(data);
    if (include_len != 1) {
        return;
    }
    if (clang_File_isEqual(included_file, ictx->target_file)) {
        return;  // защита от самовключения
    }
    // canonicalize тут же, чтобы совпадало с path_to_file_node_id, который
    // строит graph_builder.cpp тем же std::filesystem::weakly_canonical
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(
        to_std_string(clang_getFileName(included_file)), ec);
    if (!ec) {
        ictx->direct_include_paths->insert(canonical.string());
    }
}

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
    CXType param_type = clang_getCursorType(cursor);
    param["type"] = to_std_string(clang_getTypeSpelling(param_type));
    param["type_kind"] = classify_type_kind(param_type);
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
        node.metadata["field_type_kind"] = classify_type_kind(field_type);
        node.metadata["usr"] = to_std_string(clang_getCursorUSR(cursor));
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
            CXType return_type = clang_getCursorResultType(cursor);
            node.metadata["return_type"] = to_std_string(clang_getTypeSpelling(return_type));
            node.metadata["return_type_kind"] = classify_type_kind(return_type);
        }
        node.metadata["visibility"] = access_specifier_to_string(clang_getCXXAccessSpecifier(cursor));

        std::string usr = to_std_string(clang_getCursorUSR(cursor));
        node.metadata["usr"] = usr;
        // если у метода есть тело прямо тут (in-class definition), заодно
        // соберём вызовы из него; для чисто объявленных методов это
        // no-op (нет тела - clang_visitChildren ничего не найдёт)
        CallCollectContext cctx{usr, mctx->ctx->pending_calls};
        clang_visitChildren(cursor, &visit_for_calls, &cctx);

        DataFlowContext dctx{usr, AccessMode::Read, mctx->ctx->pending_reads, mctx->ctx->pending_writes};
        clang_visitChildren(cursor, &visit_for_dataflow, &dctx);
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

    if (kind == CXCursor_CXXMethod || kind == CXCursor_Constructor || kind == CXCursor_Destructor) {
        if (clang_isCursorDefinition(cursor)) {
            CXSourceLocation def_loc = clang_getCursorLocation(cursor);
            CXFile def_file;
            unsigned def_line = 0, def_col = 0, def_offset = 0;
            clang_getExpansionLocation(def_loc, &def_file, &def_line, &def_col, &def_offset);
            if (clang_File_isEqual(def_file, ctx->target_file)) {
                std::string usr = to_std_string(clang_getCursorUSR(cursor));
                CallCollectContext cctx{usr, ctx->pending_calls};
                clang_visitChildren(cursor, &visit_for_calls, &cctx);

                DataFlowContext dctx{usr, AccessMode::Read, ctx->pending_reads, ctx->pending_writes};
                clang_visitChildren(cursor, &visit_for_dataflow, &dctx);
            }
        }
        return CXChildVisit_Continue;
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

    if (kind == CXCursor_EnumDecl) {
        node.metadata["is_scoped"] = clang_EnumDecl_isScoped(cursor) != 0;
        node.metadata["underlying_type"] =
            to_std_string(clang_getTypeSpelling(clang_getEnumDeclIntegerType(cursor)));
    }

    if (kind == CXCursor_FunctionDecl) {
        node.metadata["parameters"] = collect_parameters(cursor);
        CXType return_type = clang_getCursorResultType(cursor);
        node.metadata["return_type"] = to_std_string(clang_getTypeSpelling(return_type));
        node.metadata["return_type_kind"] = classify_type_kind(return_type);

        std::string usr = to_std_string(clang_getCursorUSR(cursor));
        node.metadata["usr"] = usr;
        // no-op на голом объявлении без тела (нет CallExpr - нечего искать)
        CallCollectContext cctx{usr, ctx->pending_calls};
        clang_visitChildren(cursor, &visit_for_calls, &cctx);

        DataFlowContext dctx{usr, AccessMode::Read, ctx->pending_reads, ctx->pending_writes};
        clang_visitChildren(cursor, &visit_for_dataflow, &dctx);
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

ParseResult LibclangSymbolParser::parseFile(const ParseInput& input) const {
    ParseResult result;

    IdGenerator local_ids;

    ClangIndexHandle index;
    ClangTUHandle tu_handle;

    std::vector<std::string> arg_storage = {"-x", clang_x_mode_, clang_std_flag_};
    std::vector<const char*> args;
    args.reserve(arg_storage.size());
    for (const auto& a : arg_storage) {
        args.push_back(a.c_str());
    }

    // SkipFunctionBodies убран - без тел негде искать CallExpr, а без них
    // не построить call graph
    const unsigned options = CXTranslationUnit_KeepGoing;

    CXErrorCode err = clang_parseTranslationUnit2(
        index.index, input.absolute_path.c_str(), args.data(), static_cast<int>(args.size()),
        /*unsaved_files=*/nullptr, /*num_unsaved_files=*/0, options, &tu_handle.tu);

    if (err != CXError_Success || tu_handle.tu == nullptr) {
        std::cerr << "warning: libclang could not parse " << input.relative_path
                   << " (CXErrorCode " << static_cast<int>(err)
                   << "), skipping symbol extraction for this file\n";
        return result;
    }

    CXFile target_file = clang_getFile(tu_handle.tu, input.absolute_path.c_str());
    CXCursor root = clang_getTranslationUnitCursor(tu_handle.tu);

    VisitContext ctx{&result.fragment,     &local_ids,
                      &input,              target_file,
                      &language_label_,    input.file_node_id,
                      &result.pending_calls, &result.pending_reads,
                      &result.pending_writes};
    clang_visitChildren(root, &visit_top_level_decl, &ctx);

    std::set<std::string> direct_include_paths;
    InclusionCollectContext ictx{target_file, &direct_include_paths};
    clang_getInclusions(tu_handle.tu, &visit_inclusion, &ictx);
    for (const std::string& path : direct_include_paths) {
        result.pending_imports.push_back(PendingImport{input.file_node_id, path});
    }

    return result;
}

}  
