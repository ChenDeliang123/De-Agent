#!/usr/bin/env bash
#
# 用 CPack 生成系统安装包（.deb / .rpm）。
#
# 用法：
#   scripts/package-deb-rpm.sh [--build-dir DIR] [--clean]
#
# 产物（dist/v<version>/）：
#   de-agent_<version>_<arch>.deb
#   de-agent-<version>-<release>.<arch>.rpm
#   SHA256SUMS（与 tar 包共用；脚本会重新生成）
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

VERSION="$(sed -n 's/^[[:space:]]*VERSION[[:space:]]\+\([0-9][0-9.]*\).*/\1/p' \
  "${ROOT_DIR}/CMakeLists.txt" | head -1)"
if [ -z "${VERSION}" ]; then
  echo "cannot determine version from CMakeLists.txt" >&2
  exit 1
fi
OUT_DIR="${DIST_DIR}/v${VERSION}"

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

# 版本目录按当前 CMake VERSION 创建/复用；已有目录不删除（RELEASE.md 等保留）。
mkdir -p "${OUT_DIR}"
if [ ! -f "${OUT_DIR}/RELEASE.md" ]; then
  echo "warn: ${OUT_DIR}/RELEASE.md not found; write release notes before publishing" >&2
fi
cpack --config "${BUILD_DIR}/CPackConfig.cmake" -G "${GENERATOR_LIST}" -B "${OUT_DIR}"

# 与 tar 包共用同一份版本目录汇总校验和。
(
  cd "${OUT_DIR}"
  find . -maxdepth 1 -type f \
    \( -name 'de-agent-*.tar.gz' -o -name 'de-agent_*.deb' -o -name 'de-agent-*.rpm' \) \
    -printf '%f\n' | sort | xargs -r sha256sum > SHA256SUMS
)
# CPack 的 staging 目录不是发布产物，清掉以保持版本目录干净。
rm -rf "${OUT_DIR}/_CPack_Packages"

echo
echo "packages in ${OUT_DIR}:"
ls -1 "${OUT_DIR}"/*.deb "${OUT_DIR}"/*.rpm 2>/dev/null || true
echo "checksums: ${OUT_DIR}/SHA256SUMS"
