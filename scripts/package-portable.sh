#!/usr/bin/env bash
#
# 构建并打包「绿色版」de agent：解包后 ./de 即可运行。
#
# 用法：
#   scripts/package-portable.sh [--with-mcp] [--build-dir DIR] [--clean]
#
# 产物：
#   dist/v<version>/de-agent-<version>-linux-<arch>.tar.gz          # 默认主包（不含 MCP）
#   dist/v<version>/de-agent-<version>-linux-<arch>.tar.gz.sha256
#   dist/v<version>/de-agent-mcp-<version>-linux-<arch>.tar.gz      # --with-mcp 时另出（可选组件）
#   dist/v<version>/SHA256SUMS                                      # 版本目录内全部产物校验和
#
# 说明：
#   - 只安装 deagent 组件（不含 a2a-protocol 的头/库）。
#   - 打包不包含 conf/conf.ini（含密钥）；首启用 `./de init` 生成。
#   - MCP 是独立子项目，默认不进主包；--with-mcp 会另出一个只含
#     share/de/mcp/ 的可选包，解到主包根即生效：
#       tar xzf de-agent-mcp-*.tar.gz -C de-agent-<version>-linux-<arch>/
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
OUT_DIR="${DIST_DIR}/v${VERSION}"
PKG_DIR="${OUT_DIR}/${PACKAGE_NAME}"

# 版本目录按当前 CMake VERSION 创建/复用；已有目录不删除（RELEASE.md 等保留）。
mkdir -p "${OUT_DIR}"
if [ ! -f "${OUT_DIR}/RELEASE.md" ]; then
  echo "warn: ${OUT_DIR}/RELEASE.md not found; write release notes before publishing" >&2
fi

echo "packaging ${PACKAGE_NAME} -> ${OUT_DIR}"

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

tar -C "${OUT_DIR}" -czf "${OUT_DIR}/${PACKAGE_NAME}.tar.gz" "${PACKAGE_NAME}"
(
  cd "${OUT_DIR}"
  sha256sum "${PACKAGE_NAME}.tar.gz" > "${PACKAGE_NAME}.tar.gz.sha256"
)
# 清掉解开的 staging 目录，版本目录只留产物（tar.gz + sha256）。
rm -rf "${PKG_DIR}"

echo
echo "artifact: ${OUT_DIR}/${PACKAGE_NAME}.tar.gz"
echo "checksum: ${OUT_DIR}/${PACKAGE_NAME}.tar.gz.sha256"
du -h "${OUT_DIR}/${PACKAGE_NAME}.tar.gz" | awk '{print "size:     " $1}'

# ---------------------------------------------------------------------------
# 可选组件：MCP Server
#
# 独立打包，不混进主包：产物根目录为 share/de/mcp/，解到主包根即可：
#   tar xzf de-agent-mcp-<ver>-linux-<arch>.tar.gz -C de-agent-<ver>-linux-<arch>/
# 后端会从 <prefix>/share/de/mcp/{mcp_server,plugins} 自动发现（见 M2a）。
# ---------------------------------------------------------------------------
if [ "${WITH_MCP}" -eq 1 ]; then
  MCP_BIN="${ROOT_DIR}/vendor/mcp-extension-platform/build/mcp_server/mcp_server"
  MCP_PLUGINS="${ROOT_DIR}/vendor/mcp-extension-platform/mcp_server/plugins"
  if [ -x "${MCP_BIN}" ] && [ -d "${MCP_PLUGINS}" ]; then
    MCP_STAGE="${OUT_DIR}/.mcp-stage"
    rm -rf "${MCP_STAGE}"
    mkdir -p "${MCP_STAGE}/share/de/mcp"
    cp "${MCP_BIN}" "${MCP_STAGE}/share/de/mcp/"
    cp -r "${MCP_PLUGINS}" "${MCP_STAGE}/share/de/mcp/plugins"
    # 排除 Python 缓存：体积浪费且跨机 Python 版本可能不兼容。
    find "${MCP_STAGE}/share/de/mcp/plugins" -type d -name '__pycache__' \
      -prune -exec rm -rf {} +
    find "${MCP_STAGE}/share/de/mcp/plugins" -type f -name '*.pyc' -delete

    MCP_PACKAGE_NAME="de-agent-mcp-${VERSION}-linux-${ARCH}"
    tar -C "${MCP_STAGE}" -czf "${OUT_DIR}/${MCP_PACKAGE_NAME}.tar.gz" share
    (
      cd "${OUT_DIR}"
      sha256sum "${MCP_PACKAGE_NAME}.tar.gz" > "${MCP_PACKAGE_NAME}.tar.gz.sha256"
    )
    rm -rf "${MCP_STAGE}"

    echo
    echo "mcp add-on: ${OUT_DIR}/${MCP_PACKAGE_NAME}.tar.gz"
    echo "  install:  tar xzf ${MCP_PACKAGE_NAME}.tar.gz -C ${PACKAGE_NAME}/"
    du -h "${OUT_DIR}/${MCP_PACKAGE_NAME}.tar.gz" | awk '{print "  size:     " $1}'
  else
    echo "warn: --with-mcp but MCP binary/plugins not found under vendor/; skipped" >&2
    echo "      build it first: cmake -S vendor/mcp-extension-platform -B vendor/mcp-extension-platform/build && cmake --build vendor/mcp-extension-platform/build -j" >&2
  fi
fi

# 版本目录汇总校验和；deb/rpm 脚本随后会重新生成，纳入全部产物。
(
  cd "${OUT_DIR}"
  find . -maxdepth 1 -type f \
    \( -name 'de-agent-*.tar.gz' -o -name 'de-agent_*.deb' -o -name 'de-agent-*.rpm' \) \
    -printf '%f\n' | sort | xargs -r sha256sum > SHA256SUMS
)
echo "checksums: ${OUT_DIR}/SHA256SUMS"
