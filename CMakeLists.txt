cmake_minimum_required(VERSION 3.24)
project(editing_tools LANGUAGES CXX)

option(ET_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(ET_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

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
  src/tools/analysis.cpp
  src/tools/transcribe.cpp
  src/tools/all.cpp
  src/cli/cli.cpp
  src/tools/timeline.cpp)
target_include_directories(et_core PUBLIC src)
target_link_libraries(et_core PUBLIC nlohmann_json::nlohmann_json)
target_compile_options(et_core PRIVATE -Wall -Wextra)

add_executable(editing-tools src/main.cpp)
target_link_libraries(editing-tools PRIVATE et_core)

# editing-agent: the standalone AI editor (an agent loop + model providers on top of the same tools).
add_library(et_agent
  src/agent/http.cpp
  src/agent/openai_chat.cpp
  src/agent/anthropic.cpp
  src/agent/openai_responses.cpp
  src/agent/codex_auth.cpp
  src/agent/providers.cpp
  src/agent/policy.cpp
  src/agent/toolbox.cpp
  src/agent/prompt.cpp
  src/agent/loop.cpp
  src/agent/session.cpp
  src/agent/review.cpp
  src/util/utf8.cpp)
target_include_directories(et_agent PUBLIC src)
target_link_libraries(et_agent PUBLIC et_core nlohmann_json::nlohmann_json)
target_compile_options(et_agent PRIVATE -Wall -Wextra)

add_executable(editing-agent src/agent/main.cpp)
target_link_libraries(editing-agent PRIVATE et_agent)

enable_testing()
add_executable(et_tests tests/test_mcp.cpp tests/test_media.cpp tests/test_timeline.cpp tests/test_subtitles.cpp tests/test_color.cpp tests/test_schemas.cpp tests/test_cli.cpp tests/test_agent.cpp tests/test_agent_anthropic.cpp tests/test_agent_codex.cpp tests/test_analysis.cpp tests/test_agent_http.cpp tests/test_transcribe.cpp)
target_link_libraries(et_tests PRIVATE et_agent Catch2::Catch2WithMain)
target_compile_definitions(et_tests PRIVATE EDITING_AGENT_BIN="$<TARGET_FILE:editing-agent>")
add_dependencies(et_tests editing-agent)

include(CTest)
include(Catch)
catch_discover_tests(et_tests)

install(TARGETS editing-tools editing-agent RUNTIME DESTINATION bin)
