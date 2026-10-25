# MinGW-w64 本机构建工具链（winlibs 发行版）。
#
# 用法：
#   cmake --preset windows-mingw-debug
# 或显式指定：
#   cmake -S . -B build-mingw -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake
#
# 可用环境变量覆盖默认路径：
#   MINGW_ROOT  MinGW-w64 根目录（默认 D:/Tools/mingw64）
#   CURL_ROOT   静态 libcurl 安装前缀（默认 D:/Tools/curl-mingw）
if(NOT DEFINED MINGW_ROOT OR MINGW_ROOT STREQUAL "")
  if(DEFINED ENV{MINGW_ROOT} AND NOT "$ENV{MINGW_ROOT}" STREQUAL "")
    set(MINGW_ROOT "$ENV{MINGW_ROOT}" CACHE PATH "MinGW-w64 root" FORCE)
  else()
    set(MINGW_ROOT "D:/Tools/mingw64" CACHE PATH "MinGW-w64 root")
  endif()
endif()

set(CMAKE_C_COMPILER "${MINGW_ROOT}/bin/gcc.exe")
set(CMAKE_CXX_COMPILER "${MINGW_ROOT}/bin/g++.exe")
set(CMAKE_RC_COMPILER "${MINGW_ROOT}/bin/windres.exe")
set(CMAKE_SYSTEM_NAME Windows)

# 本机没有 vcpkg；a2a 依赖的 libcurl 由 scripts/setup-curl-mingw.ps1 构建并安装。
if(NOT DEFINED CURL_ROOT OR CURL_ROOT STREQUAL "")
  if(DEFINED ENV{CURL_ROOT} AND NOT "$ENV{CURL_ROOT}" STREQUAL "")
    set(CURL_ROOT "$ENV{CURL_ROOT}" CACHE PATH "libcurl prefix" FORCE)
  else()
    set(CURL_ROOT "D:/Tools/curl-mingw" CACHE PATH "libcurl prefix")
  endif()
endif()

# 让 find_package(CURL) 优先使用 libcurl 自带的 CMake config：
# 静态库需要把 ws2_32/crypt32 等系统库一起带出来。
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON CACHE BOOL "" FORCE)

# CMake 自带的 libcurl/GnuTLS 没有 CA 信任锚，FetchContent 从 GitHub 下载会
# 报 "certificate signer not trusted"。本机预先用 curl.exe 下载好依赖，按
# FETCHCONTENT_SOURCE_DIR_* 直接复用；没有这些目录时仍走联网下载。
if(NOT DEFINED AIAGENT_DEPS_DIR OR AIAGENT_DEPS_DIR STREQUAL "")
  if(DEFINED ENV{AIAGENT_DEPS_DIR} AND NOT "$ENV{AIAGENT_DEPS_DIR}" STREQUAL "")
    set(AIAGENT_DEPS_DIR "$ENV{AIAGENT_DEPS_DIR}" CACHE PATH "Offline deps root")
  else()
    set(AIAGENT_DEPS_DIR "D:/Tools/aiagent-deps" CACHE PATH "Offline deps root")
  endif()
endif()

set(_httplib_dir "${AIAGENT_DEPS_DIR}/cpp-httplib-0.18.3")
if(EXISTS "${_httplib_dir}/httplib.h")
  set(FETCHCONTENT_SOURCE_DIR_CPP_HTTPLIB "${_httplib_dir}" CACHE PATH "" FORCE)
endif()
set(_sqlite_dir "${AIAGENT_DEPS_DIR}/sqlite-amalgamation-3490100")
if(EXISTS "${_sqlite_dir}/sqlite3.c")
  set(FETCHCONTENT_SOURCE_DIR_SQLITE3 "${_sqlite_dir}" CACHE PATH "" FORCE)
endif()
set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)
