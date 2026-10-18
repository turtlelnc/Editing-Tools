cmake_minimum_required(VERSION 3.24)
project(editing_tools LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1)
FetchContent_MakeAvailable(json Catch2)

add_library(et_core
  src/mcp/server.cpp
  src/process.cpp
  src/model/timeline.cpp
  src/compiler/ffmpeg_graph.cpp
  src/render/render.cpp
  src/tools/media.cpp
  src/subtitles/ass.cpp
  src/tools/subtitles.cpp
  src/tools/color.cpp
  src/tools/all.cpp
  src/cli/cli.cpp
  src/tools/timeline.cpp)
target_include_directories(et_core PUBLIC src)
target_link_libraries(et_core PUBLIC nlohmann_json::nlohmann_json)
target_compile_options(et_core PRIVATE -Wall -Wextra)

add_executable(editing-tools src/main.cpp)
target_link_libraries(editing-tools PRIVATE et_core)

enable_testing()
add_executable(et_tests tests/test_mcp.cpp tests/test_media.cpp tests/test_timeline.cpp tests/test_subtitles.cpp tests/test_color.cpp tests/test_schemas.cpp tests/test_cli.cpp)
target_link_libraries(et_tests PRIVATE et_core Catch2::Catch2WithMain)
include(CTest)
include(Catch)
catch_discover_tests(et_tests)

install(TARGETS editing-tools RUNTIME DESTINATION bin)
