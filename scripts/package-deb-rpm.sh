#!/usr/bin/env bash
#
# 用 CPack 生成系统安装包（.deb / .rpm）。
#
# 用法：
#   scripts/package-deb-rpm.sh [--build-dir DIR] [--clean]
#
# 产物（dist/）：
#   de-agent_<version>-<release>_<arch>.deb
#   de-agent-<version>-<release>.<arch>.rpm
#
# 说明：
#   - 只包含 deagent 组件（bin/de、bin/aiagent_app、lib/de/cli、share/de/conf）。
#   - 装了哪个工具链就出哪种包：有 dpkg-deb 出 deb，有 rpmbuild 出 rpm；
#     两者都没有则报错。rpm 出包需要 rpm-build（apt: rpm / dnf: rpm-build）。
#   - 依赖（deb）：libcurl4(SHLIBDEPS 自动) + nodejs(>=18) + curl；
#     推荐 python3（MCP Python 插件用）。
set -euo pipefail

SCRIPT_SOURCE="${BASH_SOURCE[0]}"
while [ -L "${SCRIPT_SOURCE}" ]; do
  SCRIPT_DIR="$(cd -P "$(dirname "${SCRIPT_SOURCE}")" && pwd)"
  SCRIPT_SOURCE="$(readlink "${SCRIPT_SOURCE}")"
  case "${SCRIPT_SOURCE}" in
    /*) ;;
    *) SCRIPT_SOURCE="${SCRIPT_DIR}/${SCRIPT_SOURCE}" ;;
  esac
done
ROOT_DIR="$(cd -P "$(dirname "${SCRIPT_SOURCE}")/.." && pwd)"

BUILD_DIR="${ROOT_DIR}/build-release"
DIST_DIR="${ROOT_DIR}/dist"
CLEAN=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --clean)
      CLEAN=1
      shift
      ;;
    -h|--help)
      sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "unknown option: $1" >&2
      exit 1
      ;;
  esac
done

GENERATORS=()
if command -v dpkg-deb >/dev/null 2>&1; then
  GENERATORS+=("DEB")
fi
if command -v rpmbuild >/dev/null 2>&1; then
  GENERATORS+=("RPM")
fi
if [ "${#GENERATORS[@]}" -eq 0 ]; then
  echo "no packaging toolchain found; install dpkg (deb) and/or rpm-build (rpm)" >&2
  exit 1
fi
GENERATOR_LIST="$(IFS=';'; echo "${GENERATORS[*]}")"
echo "generators: ${GENERATOR_LIST}"

# 复用已有依赖源码，避免重复下载（与 package-portable.sh 一致）。
FETCH_ARGS=()
if [ -f "${ROOT_DIR}/build/_deps/cpp_httplib-src/httplib.h" ]; then
  FETCH_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_CPP_HTTPLIB=${ROOT_DIR}/build/_deps/cpp_httplib-src")
fi
if [ -f "${ROOT_DIR}/build/_deps/sqlite3-src/sqlite3.c" ]; then
  FETCH_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_SQLITE3=${ROOT_DIR}/build/_deps/sqlite3-src")
fi

if [ "${CLEAN}" -eq 1 ]; then
  rm -rf "${BUILD_DIR}"
fi

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DAIAGENT_BUILD_TESTS=OFF \
  -DAIAGENT_BUILD_EXAMPLES=OFF \
  "${FETCH_ARGS[@]}"
cmake --build "${BUILD_DIR}" -j

mkdir -p "${DIST_DIR}"
cpack --config "${BUILD_DIR}/CPackConfig.cmake" -G "${GENERATOR_LIST}" -B "${DIST_DIR}"

echo
echo "packages in ${DIST_DIR}:"
ls -1 "${DIST_DIR}"/*.deb "${DIST_DIR}"/*.rpm 2>/dev/null || true
