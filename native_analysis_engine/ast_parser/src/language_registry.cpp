#include "codegraph/parser_registry.hpp"

#include "c_parser.hpp"
#include "cpp_parser.hpp"
#include "go_parser.hpp"
#include "python_parser.hpp"
#include "rust_parser.hpp"

namespace codegraph {

namespace {

bool is_c_extension(const std::string& extension) {
    return extension == ".c" || extension == ".h";
}

bool is_cpp_extension(const std::string& extension) {
    return extension == ".cpp" || extension == ".cc" || extension == ".cxx" ||
           extension == ".hpp" || extension == ".hh" || extension == ".hxx";
}

bool is_rust_extension(const std::string& extension) { return extension == ".rs"; }

bool is_python_extension(const std::string& extension) { return extension == ".py"; }

bool is_go_extension(const std::string& extension) { return extension == ".go"; }

}

// это нужно для того, чтобы по расширению файла понять, какой парсер использовать
std::unique_ptr<LanguageParser> parser_for_extension(const std::string& extension) {
    if (is_c_extension(extension)) {
        return std::make_unique<CParser>();
    }
    if (is_cpp_extension(extension)) {
        return std::make_unique<CppParser>();
    }
    if (is_rust_extension(extension)) {
        return std::make_unique<RustParser>();
    }
    if (is_python_extension(extension)) {
        return std::make_unique<PythonParser>();
    }
    if (is_go_extension(extension)) {
        return std::make_unique<GoParser>();
    }
    return nullptr;
}

// это нужно для того, чтобы по расширению файла понять, какой язык использовать
std::optional<std::string> language_for_extension(const std::string& extension) {
    if (is_c_extension(extension)) {
        return "C";
    }
    if (is_cpp_extension(extension)) {
        return "C++";
    }
    if (is_rust_extension(extension)) {
        return "Rust";
    }
    if (is_python_extension(extension)) {
        return "Python";
    }
    if (is_go_extension(extension)) {
        return "Go";
    }
    return std::nullopt;
}

}
