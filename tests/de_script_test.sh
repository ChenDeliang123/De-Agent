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

# purge --yes 应删除用户数据目录（卸载包不会自动删，故提供显式命令）。
if ! out="$("${DE}" purge --yes 2>&1)"; then
  echo "${out}"
  fail "de purge --yes"
fi
[ ! -e "${DE_HOME}" ] || fail "purge did not remove data dir ${DE_HOME}"

# --- 启动输出回放（TTY 下 banner 必须出现在终端，非 TTY 不污染 stdout） ------
# 真实 backend 的输出被 nohup 重定向进 backend-console.log；de 启动成功后应把
# 新增部分回放到终端（仅 TTY），否则用户永远看不到启动横幅。
mkdir -p "${DE_HOME}/conf"
printf '[deepseek]\napi_key = sk-test\nmodel = fake\n' >"${DE_HOME}/conf/conf.ini"
if ! command -v nc >/dev/null 2>&1; then
  echo "replay test skipped: 'nc' not found"
else
  FAKE_BIN="${PREFIX}/bin/aiagent_app"
  mv "${FAKE_BIN}" "${FAKE_BIN}.real"
  cat >"${FAKE_BIN}" <<'FAKE'
#!/usr/bin/env bash
# 假后端：先打印启动横幅，再在 de 传入的端口上应答 /health。
trap 'kill "${NC_PID:-}" 2>/dev/null; exit 0' TERM INT
PORT=18081
while [ "$#" -gt 0 ]; do
  case "$1" in
    --backend-port) PORT="$2"; shift 2 ;;
    --web-port) PORT="$2"; shift 2 ;;
    *) shift ;;
  esac
done
printf 'FAKE-STARTUP-BANNER\n'
while true; do
  printf 'HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 15\r\nConnection: close\r\n\r\n{"status":"ok"}' | nc -l -p "${PORT}" >/dev/null 2>&1 &
  NC_PID=$!
  wait "${NC_PID}" 2>/dev/null || sleep 0.2
done
FAKE
  chmod +x "${FAKE_BIN}"
  # 用非默认端口，避免和开发机上正在运行的后端冲突。
  REPLAY_PORT=18088
  "${DE}" stop --backend-port "${REPLAY_PORT}" >/dev/null 2>&1 || true
  "${DE}" restart --backend-port "${REPLAY_PORT}" >/tmp/de-replay-out.txt 2>&1 || true
  if grep -q "FAKE-STARTUP-BANNER" /tmp/de-replay-out.txt; then
    fail "replay leaked to non-tty stdout"
  fi
  grep -q "FAKE-STARTUP-BANNER" "${DE_HOME}/runtime/backend-console.log" ||
    fail "backend startup output missing from console log"

  if command -v script >/dev/null 2>&1; then
    script -qefc "${DE} restart --backend-port ${REPLAY_PORT}" /tmp/de-replay-tty.raw \
      >/dev/null 2>&1 || true
    grep -q "FAKE-STARTUP-BANNER" /tmp/de-replay-tty.raw ||
      fail "startup banner not replayed on tty"
    echo "replay test passed (tty + non-tty)"
  else
    echo "replay test skipped: 'script' not found"
  fi
  rm -f /tmp/de-replay-out.txt /tmp/de-replay-tty.raw
  "${DE}" stop --backend-port "${REPLAY_PORT}" >/dev/null 2>&1 || true
  mv "${FAKE_BIN}.real" "${FAKE_BIN}"
fi

echo "de script test passed"
