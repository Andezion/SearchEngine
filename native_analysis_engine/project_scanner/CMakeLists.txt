include(FetchContent)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.12.0
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(json)

add_library(codegraph_graph_types STATIC src/graph_types.cpp)
target_include_directories(codegraph_graph_types PUBLIC include)
target_link_libraries(codegraph_graph_types PUBLIC nlohmann_json::nlohmann_json)

add_executable(codegraph_scanner
  src/main.cpp
  src/cli_options.cpp
  src/directory_walker.cpp
  src/graph_builder.cpp
)

target_include_directories(codegraph_scanner PRIVATE include)
target_link_libraries(codegraph_scanner PRIVATE codegraph_graph_types ast_parser)

if(MSVC)
  target_compile_options(codegraph_scanner PRIVATE /W4)
else()
  target_compile_options(codegraph_scanner PRIVATE -Wall -Wextra)
endif()
