cmake_minimum_required(VERSION 3.21)

project(
  ai-agent-cpp
  VERSION 0.1.0
  DESCRIPTION "AI Agent runtime in C++, reusing a2a-protocol and mcp-extension-platform"
  LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(AIAGENT_BUILD_TESTS "Build AI Agent smoke/integration tests" ON)

# ---------------------------------------------------------------------------
# a2a-protocol 复用
#
# M0 阶段直接复用本地 vendor/a2a-protocol（独立仓库），不复制源码。
# 后续发布前可再切换为 Git submodule / FetchContent。
# ---------------------------------------------------------------------------
set(AIAGENT_A2A_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/a2a-protocol"
    CACHE PATH
    "Path to a local checkout of the a2a-protocol C++ library")

if(NOT EXISTS "${AIAGENT_A2A_SOURCE_DIR}/CMakeLists.txt")
  message(
    FATAL_ERROR
    "a2a-protocol not found at ${AIAGENT_A2A_SOURCE_DIR}. "
    "Clone https://gitee.com/chen_dl/a2a-protocol.git into "
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/a2a-protocol "
    "or set -DAIAGENT_A2A_SOURCE_DIR=<path>."
  )
endif()

# 子项目只贡献库，不重复构建其示例/测试。
set(A2A_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(A2A_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# M0 只需要 HTTP/SSE 服务端；SQLite 持久化在 M2/M6 再打开。
set(A2A_BUILD_HTTP_SERVER ON CACHE BOOL "" FORCE)
set(A2A_BUILD_SQLITE_STORE OFF CACHE BOOL "" FORCE)

# 若本机已有 a2a-protocol 的历史构建缓存，优先复用 cpp-httplib 源码，
# 避免每次配置都重新下载依赖。
if(EXISTS "${AIAGENT_A2A_SOURCE_DIR}/build/_deps/cpp_httplib-src/httplib.h")
  set(FETCHCONTENT_SOURCE_DIR_CPP_HTTPLIB
      "${AIAGENT_A2A_SOURCE_DIR}/build/_deps/cpp_httplib-src"
      CACHE PATH "" FORCE)
endif()

add_subdirectory("${AIAGENT_A2A_SOURCE_DIR}" a2a-protocol)

# ---------------------------------------------------------------------------
# 本项目可执行文件
# ---------------------------------------------------------------------------
add_executable(aiagent_app src/main.cpp)
target_include_directories(aiagent_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(aiagent_app PRIVATE a2a::a2a)

if(MSVC)
  target_compile_options(aiagent_app PRIVATE /W4 /permissive-)
else()
  target_compile_options(aiagent_app PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
if(AIAGENT_BUILD_TESTS)
  enable_testing()

  add_executable(m0_smoke_test tests/m0_smoke_test.cpp)
  target_include_directories(m0_smoke_test PRIVATE
                             ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(m0_smoke_test PRIVATE a2a::a2a)
  add_test(NAME aiagent_m0_smoke COMMAND m0_smoke_test)
endif()
