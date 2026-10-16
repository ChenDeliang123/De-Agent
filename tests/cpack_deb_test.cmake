# CPack DEB 冒烟测试：真正生成一次 .deb，校验内容布局。
#
# 仅在 ctest 运行时已构建产物、且本机有 dpkg-deb 时有意义（CMakeLists 已做 find_program）。
#
# 依赖变量（由 CMakeLists 通过 -D 传入）：
#   AIAGENT_BUILD_DIR   含 CPackConfig.cmake 的构建目录
#   AIAGENT_CPACK_STAGE cpack 输出目录（临时）
#   AIAGENT_CPACK       cpack 可执行文件
#   AIAGENT_DPKG_DEB    dpkg-deb 可执行文件

cmake_minimum_required(VERSION 3.21)

foreach(
  var
  AIAGENT_BUILD_DIR
  AIAGENT_CPACK_STAGE
  AIAGENT_CPACK
  AIAGENT_DPKG_DEB)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "required variable ${var} is not set")
  endif()
endforeach()

set(stage "${AIAGENT_CPACK_STAGE}")
file(REMOVE_RECURSE "${stage}")
file(MAKE_DIRECTORY "${stage}")

execute_process(
  COMMAND "${AIAGENT_CPACK}" --config "${AIAGENT_BUILD_DIR}/CPackConfig.cmake"
          -G DEB -B "${stage}"
  RESULT_VARIABLE cpack_result
  OUTPUT_VARIABLE cpack_out
  ERROR_VARIABLE cpack_err)
if(NOT cpack_result EQUAL 0)
  message(
    FATAL_ERROR "cpack -G DEB failed (${cpack_result}):\n${cpack_out}\n${cpack_err}"
  )
endif()

file(GLOB debs "${stage}/*.deb")
list(LENGTH debs deb_count)
if(deb_count EQUAL 0)
  message(FATAL_ERROR "cpack produced no .deb\n${cpack_out}")
endif()
list(GET debs 0 deb)

execute_process(
  COMMAND "${AIAGENT_DPKG_DEB}" -c "${deb}"
  RESULT_VARIABLE list_result
  OUTPUT_VARIABLE contents)
if(NOT list_result EQUAL 0)
  message(FATAL_ERROR "dpkg-deb -c failed for ${deb}")
endif()

set(required
    "usr/bin/de"
    "usr/bin/aiagent_app"
    "usr/lib/de/cli/cli.mjs"
    "usr/share/de/conf/app.ini.example")
foreach(path ${required})
  if(NOT contents MATCHES "${path}")
    message(FATAL_ERROR "deb missing ${path}:\n${contents}")
  endif()
endforeach()

# 只应包含本项目产物，不得带入 a2a-protocol 的头/库。
if(contents MATCHES "include/a2a" OR contents MATCHES "liba2a")
  message(FATAL_ERROR "deb unexpectedly contains a2a artifacts:\n${contents}")
endif()

message(STATUS "cpack DEB OK: ${deb}")
