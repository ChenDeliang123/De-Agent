# 安装布局冒烟测试（ctest）。
#
# 只安装 deagent 组件到临时 stage 目录，校验打包所需的产物路径齐全，
# 并检查生成的 version.hpp 确实带上了 project(VERSION)。
#
# 依赖变量（由 CMakeLists 通过 -D 传入）：
#   AIAGENT_BUILD_DIR      构建目录
#   AIAGENT_INSTALL_STAGE  临时安装根
#   AIAGENT_INSTALL_BINDIR / LIBDIR / DATADIR  安装子目录名

cmake_minimum_required(VERSION 3.21)

foreach(var AIAGENT_BUILD_DIR AIAGENT_INSTALL_STAGE AIAGENT_INSTALL_BINDIR
            AIAGENT_INSTALL_LIBDIR AIAGENT_INSTALL_DATADIR)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "required variable ${var} is not set")
  endif()
endforeach()

set(stage "${AIAGENT_INSTALL_STAGE}")
file(REMOVE_RECURSE "${stage}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${AIAGENT_BUILD_DIR}" --component
          deagent --prefix "${stage}"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_out
  ERROR_VARIABLE install_err)
if(NOT install_result EQUAL 0)
  message(
    FATAL_ERROR
      "cmake --install --component deagent failed (${install_result}):\n${install_out}\n${install_err}"
  )
endif()

set(expected
    "${stage}/${AIAGENT_INSTALL_BINDIR}/aiagent_app"
    "${stage}/${AIAGENT_INSTALL_BINDIR}/de"
    "${stage}/${AIAGENT_INSTALL_LIBDIR}/de/cli/cli.mjs"
    "${stage}/${AIAGENT_INSTALL_DATADIR}/de/conf/app.ini.example"
    "${stage}/${AIAGENT_INSTALL_DATADIR}/de/conf/cli.ini.example"
    # 子 Agent 提示词：缺失时后端会拒绝启动（不做无提示词的静默降级），
    # 所以安装布局必须包含它们。
    "${stage}/${AIAGENT_INSTALL_DATADIR}/de/prompts/general.md"
    "${stage}/${AIAGENT_INSTALL_DATADIR}/de/prompts/explorer.md")

foreach(path ${expected})
  if(NOT EXISTS "${path}")
    message(
      FATAL_ERROR
        "missing install artifact: ${path}\ninstall output:\n${install_out}")
  endif()
endforeach()

# 打包不应带入测试目录与本地运行时产物。
if(EXISTS "${stage}/${AIAGENT_INSTALL_LIBDIR}/de/cli/tests")
  message(FATAL_ERROR "cli tests directory should not be installed")
endif()
if(EXISTS "${stage}/${AIAGENT_INSTALL_LIBDIR}/de/cli/.runtime")
  message(FATAL_ERROR "cli .runtime directory should not be installed")
endif()

set(version_header "${AIAGENT_BUILD_DIR}/generated/aiagent/version.hpp")
file(READ "${version_header}" version_content)
if(NOT version_content MATCHES "kVersion = \"[0-9]+\\.[0-9]+\\.[0-9]+\"")
  message(FATAL_ERROR
          "generated version header has no semantic version: ${version_header}")
endif()

message(STATUS "packaging install layout OK: ${stage}")
