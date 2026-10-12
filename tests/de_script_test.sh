#!/usr/bin/env bash
#
# de 启动器脚本冒烟测试（ctest）。
#
# 把 deagent 组件安装到一个临时 prefix（模拟安装态），再用 DE_HOME 指向临时数据目录，
# 校验 de init / doctor / help 与布局探测。
#
# 参数：<cmake> <build-dir> <source-dir>
set -euo pipefail

CMAKE="$1"
BUILD_DIR="$2"
SRC_DIR="$3"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

PREFIX="${WORK}/prefix"
"${CMAKE}" --install "${BUILD_DIR}" --component deagent --prefix "${PREFIX}" >/dev/null

DE="${PREFIX}/bin/de"
if [ ! -x "${DE}" ]; then
  echo "FAILED: installed de not executable: ${DE}" >&2
  exit 1
fi

export DE_HOME="${WORK}/data"
export HOME="${WORK}/home"
mkdir -p "${HOME}"
# 从非仓库 cwd 运行，验证不依赖源码树。
cd "${WORK}"

fail() {
  echo "FAILED: $*" >&2
  exit 1
}

"${DE}" help >/dev/null 2>&1 || fail "de help"

if ! out="$("${DE}" init 2>&1)"; then
  echo "${out}"
  fail "de init"
fi
[ -f "${DE_HOME}/conf/conf.ini" ] || fail "init did not create conf.ini"
[ -f "${DE_HOME}/conf/cli.ini" ] || fail "init did not create cli.ini"

# 二次 init 不得覆盖已有配置。
echo "; keep-me" >>"${DE_HOME}/conf/conf.ini"
"${DE}" init >/dev/null 2>&1 || fail "de init (second run)"
grep -q "keep-me" "${DE_HOME}/conf/conf.ini" || fail "init overwrote existing config"

if ! out="$("${DE}" doctor 2>&1)"; then
  echo "${out}"
  fail "de doctor"
fi

# 布局探测：安装态应识别为 installed，并把数据目录指到 DE_HOME。
# 注意：不要用 `de doctor | grep -q`——grep -q 提前退出会让 de 收到 SIGPIPE，
# 在 set -o pipefail 下被误判为失败。
case "${out}" in
  *"layout:      installed"*) ;;
  *) fail "doctor did not report installed layout" ;;
esac
case "${out}" in
  *"data dir:    ${DE_HOME}"*) ;;
  *) fail "doctor did not resolve DE_HOME data dir" ;;
esac

echo "de script test passed"
