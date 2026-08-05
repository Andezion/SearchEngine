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

add_library(ast_parser STATIC
  src/language_registry.cpp
  src/graph_merge.cpp
  src/libclang_symbol_parser.cpp
  src/c_parser.cpp
  src/cpp_parser.cpp
)

target_include_directories(ast_parser
  PUBLIC include
  PRIVATE src)
target_link_libraries(ast_parser
  PUBLIC codegraph_graph_types
  PRIVATE libclang_imported)

if(MSVC)
  target_compile_options(ast_parser PRIVATE /W4)
else()
  target_compile_options(ast_parser PRIVATE -Wall -Wextra)
endif()
