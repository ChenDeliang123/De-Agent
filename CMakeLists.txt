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

find_package(Threads REQUIRED)

include(GNUInstallDirs)

# 版本头单一来源：由 project(VERSION) 生成到构建目录，避免手写版本号漂移。
# 模板：include/aiagent/version.hpp.in
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/aiagent/version.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/aiagent/version.hpp"
  @ONLY)
add_library(aiagent_version INTERFACE)
target_include_directories(aiagent_version
                           INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/generated")

option(AIAGENT_BUILD_TESTS "Build AI Agent smoke/integration tests" ON)
option(AIAGENT_BUILD_EXAMPLES "Build AI Agent example programs" ON)

# ---------------------------------------------------------------------------
# a2a-protocol 复用
#
# vendor/a2a-protocol 是根仓库的 Git submodule（Gitee 独立仓库），
# 这里直接作为子项目编译，不复制源码。
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
# HTTP/SSE 服务端与 SQLite 任务持久化。
set(A2A_BUILD_HTTP_SERVER ON CACHE BOOL "" FORCE)
set(A2A_BUILD_SQLITE_STORE ON CACHE BOOL "" FORCE)

# 若本机已有 cpp-httplib 的历史构建缓存，优先复用源码，
# 避免每次配置都重新下载依赖。
if(DEFINED FETCHCONTENT_SOURCE_DIR_CPP_HTTPLIB AND NOT EXISTS
   "${FETCHCONTENT_SOURCE_DIR_CPP_HTTPLIB}/httplib.h")
  unset(FETCHCONTENT_SOURCE_DIR_CPP_HTTPLIB CACHE)
endif()
if(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/_deps/cpp_httplib-src/httplib.h")
  set(FETCHCONTENT_SOURCE_DIR_CPP_HTTPLIB
      "${CMAKE_CURRENT_BINARY_DIR}/_deps/cpp_httplib-src"
      CACHE PATH "" FORCE)
endif()
if(DEFINED FETCHCONTENT_SOURCE_DIR_SQLITE3 AND NOT EXISTS
   "${FETCHCONTENT_SOURCE_DIR_SQLITE3}/sqlite3.c")
  unset(FETCHCONTENT_SOURCE_DIR_SQLITE3 CACHE)
endif()
if(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/_deps/sqlite3-src/sqlite3.c")
  set(FETCHCONTENT_SOURCE_DIR_SQLITE3
      "${CMAKE_CURRENT_BINARY_DIR}/_deps/sqlite3-src"
      CACHE PATH "" FORCE)
endif()

add_subdirectory("${AIAGENT_A2A_SOURCE_DIR}" a2a-protocol)

# ---------------------------------------------------------------------------
# 本项目核心库
# ---------------------------------------------------------------------------
add_library(aiagent_log STATIC src/log.cpp)
target_include_directories(aiagent_log PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# 生成的 version.hpp 通过 aiagent_version 传递到所有下游目标。
target_link_libraries(aiagent_log PUBLIC aiagent_version)

if(MSVC)
  target_compile_options(aiagent_log PRIVATE /W4 /permissive-)
else()
  target_compile_options(aiagent_log PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(aiagent_llm STATIC
  src/config.cpp
  src/llm/deepseek_client.cpp
)
target_include_directories(aiagent_llm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(aiagent_llm PUBLIC aiagent_log a2a::a2a)

if(MSVC)
  target_compile_options(aiagent_llm PRIVATE /W4 /permissive-)
else()
  target_compile_options(aiagent_llm PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(aiagent_agent STATIC
  src/app_paths.cpp
  src/agent/agent_runtime.cpp
  src/agent/approval_broker.cpp
  src/agent/background_delivery.cpp
  src/agent/compaction.cpp
  src/agent/history_cache.cpp
  src/agent/message_inbox.cpp
  src/agent/project_instructions.cpp
  src/agent/session_history.cpp
  src/agent/session_meta_store.cpp
  src/agent/sub_agent.cpp
  src/agent/system_prompt.cpp
  src/permissions/bash_policy.cpp
  src/permissions/permission_policy.cpp
  src/permissions/permission_store.cpp
  src/permissions/project_config.cpp
  src/permissions/trust_store.cpp
  src/tools/run_command.cpp
  src/tools/local_tools.cpp
  src/tools/composite_tool_provider.cpp
  src/tools/filtered_tool_provider.cpp
  src/tools/sub_agent_tool.cpp
)
target_include_directories(aiagent_agent PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(aiagent_agent PUBLIC aiagent_llm a2a::a2a)

if(MSVC)
  target_compile_options(aiagent_agent PRIVATE /W4 /permissive-)
else()
  target_compile_options(aiagent_agent PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(aiagent_mcp STATIC src/mcp/mcp_client_bridge.cpp)
target_include_directories(aiagent_mcp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(aiagent_mcp PUBLIC aiagent_llm a2a::a2a Threads::Threads)

if(MSVC)
  target_compile_options(aiagent_mcp PRIVATE /W4 /permissive-)
else()
  target_compile_options(aiagent_mcp PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(aiagent_web STATIC src/web/web_gateway.cpp)
if(DEFINED FETCHCONTENT_SOURCE_DIR_CPP_HTTPLIB AND EXISTS
   "${FETCHCONTENT_SOURCE_DIR_CPP_HTTPLIB}/httplib.h")
  set(AIAGENT_HTTPLIB_INCLUDE_DIR "${FETCHCONTENT_SOURCE_DIR_CPP_HTTPLIB}")
else()
  set(AIAGENT_HTTPLIB_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/cpp_httplib-src")
endif()
target_include_directories(
  aiagent_web
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${AIAGENT_HTTPLIB_INCLUDE_DIR}
)
target_link_libraries(aiagent_web PUBLIC aiagent_agent a2a::a2a Threads::Threads)

if(MSVC)
  target_compile_options(aiagent_web PRIVATE /W4 /permissive-)
else()
  target_compile_options(aiagent_web PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# 本项目可执行文件
# ---------------------------------------------------------------------------
add_executable(aiagent_app src/main.cpp)
target_include_directories(aiagent_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(aiagent_app PRIVATE
                      aiagent_agent aiagent_mcp aiagent_web aiagent_llm
                      aiagent_log a2a::a2a)

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
  target_link_libraries(m0_smoke_test PRIVATE a2a::a2a aiagent_version)
  add_test(NAME aiagent_m0_smoke COMMAND m0_smoke_test)

  # 版本单一来源：生成头中的 kVersion 必须等于 project(VERSION)。
  add_executable(version_test tests/version_test.cpp)
  target_include_directories(version_test PRIVATE
                             ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(version_test PRIVATE aiagent_log)
  target_compile_definitions(version_test PRIVATE
                             AIAGENT_EXPECTED_VERSION="${PROJECT_VERSION}")
  add_test(NAME aiagent_version COMMAND version_test)

  # 安装布局冒烟测试：只安装 deagent 组件，校验产物路径。
  add_test(
    NAME aiagent_packaging_install
    COMMAND
      ${CMAKE_COMMAND} -DAIAGENT_BUILD_DIR=${CMAKE_BINARY_DIR}
      -DAIAGENT_INSTALL_STAGE=${CMAKE_BINARY_DIR}/install-stage
      -DAIAGENT_INSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}
      -DAIAGENT_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}
      -DAIAGENT_INSTALL_DATADIR=${CMAKE_INSTALL_DATADIR} -P
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/packaging_install_test.cmake)

  # de 启动器脚本冒烟测试：安装到临时 prefix，校验 init/doctor/布局。
  add_test(
    NAME aiagent_de_script
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/de_script_test.sh
            ${CMAKE_COMMAND} ${CMAKE_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR})

  # CPack DEB 冒烟测试（仅当本机同时有 cpack 与 dpkg-deb）。
  find_program(AIAGENT_CPACK_EXECUTABLE cpack)
  find_program(AIAGENT_DPKG_DEB_EXECUTABLE dpkg-deb)
  if(AIAGENT_CPACK_EXECUTABLE AND AIAGENT_DPKG_DEB_EXECUTABLE)
    add_test(
      NAME aiagent_cpack_deb
      COMMAND
        ${CMAKE_COMMAND} -DAIAGENT_BUILD_DIR=${CMAKE_BINARY_DIR}
        -DAIAGENT_CPACK_STAGE=${CMAKE_BINARY_DIR}/cpack-stage
        -DAIAGENT_CPACK=${AIAGENT_CPACK_EXECUTABLE}
        -DAIAGENT_DPKG_DEB=${AIAGENT_DPKG_DEB_EXECUTABLE} -P
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cpack_deb_test.cmake)
  endif()

  add_executable(logging_test tests/logging_test.cpp)
  target_link_libraries(logging_test PRIVATE aiagent_log aiagent_llm Threads::Threads)
  add_test(NAME aiagent_logging COMMAND logging_test)

  add_executable(llm_client_test tests/llm_client_test.cpp)
  target_link_libraries(llm_client_test PRIVATE aiagent_llm Threads::Threads)
  target_include_directories(llm_client_test PRIVATE ${AIAGENT_HTTPLIB_INCLUDE_DIR})
  add_test(NAME aiagent_llm_client COMMAND llm_client_test)

  add_executable(agent_runtime_test tests/agent_runtime_test.cpp)
  target_link_libraries(agent_runtime_test PRIVATE aiagent_agent)
  add_test(NAME aiagent_agent_runtime COMMAND agent_runtime_test)

  add_executable(system_prompt_test tests/system_prompt_test.cpp)
  target_link_libraries(system_prompt_test PRIVATE aiagent_agent)
  add_test(NAME aiagent_system_prompt COMMAND system_prompt_test)

  add_executable(history_cache_test tests/history_cache_test.cpp)
  target_link_libraries(history_cache_test PRIVATE aiagent_agent)
  add_test(NAME aiagent_history_cache COMMAND history_cache_test)

  add_executable(session_meta_store_test tests/session_meta_store_test.cpp)
  target_link_libraries(session_meta_store_test PRIVATE aiagent_agent)
  add_test(NAME aiagent_session_meta_store COMMAND session_meta_store_test)

  add_executable(permission_policy_test tests/permission_policy_test.cpp)
  target_link_libraries(permission_policy_test PRIVATE aiagent_agent)
  add_test(NAME aiagent_permission_policy COMMAND permission_policy_test)

  add_executable(permission_store_test tests/permission_store_test.cpp)
  target_link_libraries(permission_store_test PRIVATE aiagent_agent)
  add_test(NAME aiagent_permission_store COMMAND permission_store_test)

  add_executable(approval_broker_test tests/approval_broker_test.cpp)
  target_link_libraries(approval_broker_test PRIVATE aiagent_agent)
  add_test(NAME aiagent_approval_broker COMMAND approval_broker_test)

  add_executable(bash_policy_test tests/bash_policy_test.cpp)
  target_link_libraries(bash_policy_test PRIVATE aiagent_agent)
  add_test(NAME aiagent_bash_policy COMMAND bash_policy_test)

  add_executable(project_config_test tests/project_config_test.cpp)
  target_link_libraries(project_config_test PRIVATE aiagent_agent)
  add_test(NAME aiagent_project_config COMMAND project_config_test)

  add_executable(trust_store_test tests/trust_store_test.cpp)
  target_link_libraries(trust_store_test PRIVATE aiagent_agent)
  add_test(NAME aiagent_trust_store COMMAND trust_store_test)

  add_executable(app_paths_test tests/app_paths_test.cpp)
  target_link_libraries(app_paths_test PRIVATE aiagent_agent)
  add_test(NAME aiagent_app_paths COMMAND app_paths_test)

  add_executable(compaction_test tests/compaction_test.cpp)
  target_link_libraries(compaction_test PRIVATE aiagent_agent)
  add_test(NAME aiagent_compaction COMMAND compaction_test)

  add_executable(project_instructions_test tests/project_instructions_test.cpp)
  target_link_libraries(project_instructions_test PRIVATE aiagent_agent)
  add_test(NAME aiagent_project_instructions COMMAND project_instructions_test)

  add_executable(local_tools_test tests/local_tools_test.cpp)
  target_link_libraries(local_tools_test PRIVATE aiagent_agent)
  add_test(NAME aiagent_local_tools COMMAND local_tools_test)

  add_executable(sub_agent_test tests/sub_agent_test.cpp)
  target_link_libraries(sub_agent_test PRIVATE aiagent_agent)
  add_test(NAME aiagent_sub_agent COMMAND sub_agent_test)

  add_executable(background_delivery_test tests/background_delivery_test.cpp)
  target_link_libraries(background_delivery_test PRIVATE aiagent_agent)
  add_test(NAME aiagent_background_delivery COMMAND background_delivery_test)

  add_executable(message_inbox_test tests/message_inbox_test.cpp)
  target_link_libraries(message_inbox_test PRIVATE aiagent_agent)
  add_test(NAME aiagent_message_inbox COMMAND message_inbox_test)

  add_executable(fake_mcp_server tests/fake_mcp_server.cpp)
  target_link_libraries(fake_mcp_server PRIVATE aiagent_llm a2a::a2a)

  add_executable(mcp_bridge_test tests/mcp_bridge_test.cpp)
  target_link_libraries(mcp_bridge_test PRIVATE aiagent_mcp aiagent_agent)
  target_compile_definitions(mcp_bridge_test PRIVATE
                             AIAGENT_FAKE_MCP_BINARY="$<TARGET_FILE:fake_mcp_server>")
  add_test(NAME aiagent_mcp_bridge COMMAND mcp_bridge_test)

  add_executable(web_gateway_test tests/web_gateway_test.cpp)
  target_link_libraries(web_gateway_test PRIVATE aiagent_web aiagent_agent)
  target_include_directories(web_gateway_test PRIVATE ${AIAGENT_HTTPLIB_INCLUDE_DIR})
  add_test(NAME aiagent_web_gateway COMMAND web_gateway_test)
endif()

# ---------------------------------------------------------------------------
# 示例
# ---------------------------------------------------------------------------
if(AIAGENT_BUILD_EXAMPLES)
  add_executable(aiagent_deepseek_cli examples/deepseek_chat.cpp)
  target_link_libraries(aiagent_deepseek_cli PRIVATE aiagent_llm)
endif()

# ---------------------------------------------------------------------------
# 安装 / 打包
#
# 所有本项目产物都带 COMPONENT deagent，这样即使 a2a-protocol 子项目也声明了
# 自己的 install() 规则，打包时仍可只安装本项目：
#   cmake --install build --component deagent --prefix <stage>
# ---------------------------------------------------------------------------
install(TARGETS aiagent_app
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT deagent)

install(PROGRAMS "${CMAKE_CURRENT_SOURCE_DIR}/de"
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT deagent)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/cli/"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/de/cli
        COMPONENT deagent
        PATTERN "tests" EXCLUDE
        PATTERN ".runtime" EXCLUDE
        PATTERN "node_modules" EXCLUDE)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/prompts/"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/de/prompts
        COMPONENT deagent)

install(FILES
          "${CMAKE_CURRENT_SOURCE_DIR}/conf/app.ini.example"
          "${CMAKE_CURRENT_SOURCE_DIR}/conf/cli.ini.example"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/de/conf
        COMPONENT deagent)

# ---------------------------------------------------------------------------
# CPack 打包（deb / rpm）；仅在顶层工程时启用。
# ---------------------------------------------------------------------------
if(PROJECT_IS_TOP_LEVEL)
  include(cmake/Packaging.cmake)
endif()
