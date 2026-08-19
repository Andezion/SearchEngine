find_library(LIBCLANG_LIBRARY NAMES clang)
find_path(LIBCLANG_INCLUDE_DIR NAMES clang-c/Index.h)

if(NOT LIBCLANG_LIBRARY OR NOT LIBCLANG_INCLUDE_DIR)
  message(FATAL_ERROR
    "libclang development files not found (LIBCLANG_LIBRARY=${LIBCLANG_LIBRARY}, "
    "LIBCLANG_INCLUDE_DIR=${LIBCLANG_INCLUDE_DIR}). On Fedora:\n"
    "  sudo dnf install clang-devel llvm-devel")
endif()

add_library(libclang_imported SHARED IMPORTED)
set_target_properties(libclang_imported PROPERTIES
  IMPORTED_LOCATION "${LIBCLANG_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${LIBCLANG_INCLUDE_DIR}")

# Rust/Python/Go парсим через tree-sitter - ибо мы ебланы и у libclang нет грамматик для них
find_library(TREE_SITTER_LIBRARY NAMES tree-sitter)
find_path(TREE_SITTER_INCLUDE_DIR NAMES tree_sitter/api.h)
find_library(TREE_SITTER_RUST_LIBRARY NAMES tree-sitter-rust)
find_path(TREE_SITTER_RUST_INCLUDE_DIR NAMES tree_sitter/tree-sitter-rust.h)
find_library(TREE_SITTER_PYTHON_LIBRARY NAMES tree-sitter-python)
find_path(TREE_SITTER_PYTHON_INCLUDE_DIR NAMES tree_sitter/tree-sitter-python.h)
find_library(TREE_SITTER_GO_LIBRARY NAMES tree-sitter-go)
find_path(TREE_SITTER_GO_INCLUDE_DIR NAMES tree_sitter/tree-sitter-go.h)

if(NOT TREE_SITTER_LIBRARY OR NOT TREE_SITTER_INCLUDE_DIR OR
   NOT TREE_SITTER_RUST_LIBRARY OR NOT TREE_SITTER_RUST_INCLUDE_DIR OR
   NOT TREE_SITTER_PYTHON_LIBRARY OR NOT TREE_SITTER_PYTHON_INCLUDE_DIR OR
   NOT TREE_SITTER_GO_LIBRARY OR NOT TREE_SITTER_GO_INCLUDE_DIR)
  message(FATAL_ERROR
    "tree-sitter development files not found. On Fedora:\n"
    "  sudo dnf install libtree-sitter-devel libtree-sitter-rust-devel "
    "libtree-sitter-python-devel libtree-sitter-go-devel")
endif()

add_library(tree_sitter_imported SHARED IMPORTED)
set_target_properties(tree_sitter_imported PROPERTIES
  IMPORTED_LOCATION "${TREE_SITTER_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${TREE_SITTER_INCLUDE_DIR}")

add_library(tree_sitter_rust_imported SHARED IMPORTED)
set_target_properties(tree_sitter_rust_imported PROPERTIES
  IMPORTED_LOCATION "${TREE_SITTER_RUST_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${TREE_SITTER_RUST_INCLUDE_DIR}")

add_library(tree_sitter_python_imported SHARED IMPORTED)
set_target_properties(tree_sitter_python_imported PROPERTIES
  IMPORTED_LOCATION "${TREE_SITTER_PYTHON_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${TREE_SITTER_PYTHON_INCLUDE_DIR}")

add_library(tree_sitter_go_imported SHARED IMPORTED)
set_target_properties(tree_sitter_go_imported PROPERTIES
  IMPORTED_LOCATION "${TREE_SITTER_GO_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${TREE_SITTER_GO_INCLUDE_DIR}")

add_library(ast_parser STATIC
  src/language_registry.cpp
  src/graph_merge.cpp
  src/reference_resolver.cpp
  src/libclang_symbol_parser.cpp
  src/c_parser.cpp
  src/cpp_parser.cpp
  src/rust_parser.cpp
  src/python_parser.cpp
  src/go_parser.cpp
)

target_include_directories(ast_parser
  PUBLIC include
  PRIVATE src)
target_link_libraries(ast_parser
  PUBLIC codegraph_graph_types
  PRIVATE libclang_imported tree_sitter_imported tree_sitter_rust_imported
          tree_sitter_python_imported tree_sitter_go_imported)

if(MSVC)
  target_compile_options(ast_parser PRIVATE /W4)
else()
  target_compile_options(ast_parser PRIVATE -Wall -Wextra)
endif()
