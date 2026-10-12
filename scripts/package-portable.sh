#!/usr/bin/env bash
#
# 构建并打包「绿色版」de agent：解包后 ./de 即可运行。
#
# 用法：
#   scripts/package-portable.sh [--with-mcp] [--build-dir DIR] [--clean]
#
# 产物：
#   dist/de-agent-<version>-linux-<arch>.tar.gz
#   dist/de-agent-<version>-linux-<arch>.tar.gz.sha256
#
# 说明：
#   - 只安装 deagent 组件（不含 a2a-protocol 的头/库）。
#   - 打包不包含 conf/conf.ini（含密钥）；首启用 `./de init` 生成。
#   - 若存在 build/_deps 下的依赖源码，会自动复用，避免重新联网下载。
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
WITH_MCP=0
CLEAN=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --with-mcp)
      WITH_MCP=1
      shift
      ;;
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --clean)
      CLEAN=1
      shift
      ;;
    -h|--help)
      sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "unknown option: $1" >&2
      exit 1
      ;;
  esac
done

VERSION="$(sed -n 's/^[[:space:]]*VERSION[[:space:]]\+\([0-9][0-9.]*\).*/\1/p' \
  "${ROOT_DIR}/CMakeLists.txt" | head -1)"
if [ -z "${VERSION}" ]; then
  echo "cannot determine version from CMakeLists.txt" >&2
  exit 1
fi
ARCH="$(uname -m)"
PACKAGE_NAME="de-agent-${VERSION}-linux-${ARCH}"
PKG_DIR="${DIST_DIR}/${PACKAGE_NAME}"

echo "packaging ${PACKAGE_NAME}"

# 复用已有依赖源码，避免打包机重复下载。
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

rm -rf "${PKG_DIR}"
mkdir -p "${PKG_DIR}"
cmake --install "${BUILD_DIR}" --component deagent --prefix "${PKG_DIR}"

# 包根放一个 de 软链，方便 `./de`（解析软链后仍能识别安装布局）。
ln -sf "bin/de" "${PKG_DIR}/de"

if [ "${WITH_MCP}" -eq 1 ]; then
  MCP_BIN="${ROOT_DIR}/vendor/mcp-extension-platform/build/mcp_server/mcp_server"
  MCP_PLUGINS="${ROOT_DIR}/vendor/mcp-extension-platform/mcp_server/plugins"
  if [ -x "${MCP_BIN}" ]; then
    mkdir -p "${PKG_DIR}/share/de/mcp"
    cp "${MCP_BIN}" "${PKG_DIR}/share/de/mcp/"
    if [ -d "${MCP_PLUGINS}" ]; then
      cp -r "${MCP_PLUGINS}" "${PKG_DIR}/share/de/mcp/plugins"
    fi
    echo "bundled MCP server"
  else
    echo "warn: --with-mcp but ${MCP_BIN} not built; skipping MCP" >&2
  fi
fi

mkdir -p "${DIST_DIR}"
tar -C "${DIST_DIR}" -czf "${DIST_DIR}/${PACKAGE_NAME}.tar.gz" "${PACKAGE_NAME}"
(
  cd "${DIST_DIR}"
  sha256sum "${PACKAGE_NAME}.tar.gz" > "${PACKAGE_NAME}.tar.gz.sha256"
)

echo
echo "artifact: ${DIST_DIR}/${PACKAGE_NAME}.tar.gz"
echo "checksum: ${DIST_DIR}/${PACKAGE_NAME}.tar.gz.sha256"
du -h "${DIST_DIR}/${PACKAGE_NAME}.tar.gz" | awk '{print "size:     " $1}'
