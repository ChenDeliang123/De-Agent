import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import test from "node:test";

// /quit 不应让进程卡住：LineEditor.readLine 会 resume stdin 以接收按键，
// 退出主循环后必须 pause，否则处于 flowing 的 stdin 会阻止 Node 退出。
// 只有 TTY 才会走 LineEditor 分支，所以需要伪终端（util-linux 的 script）。
const scriptPath = "/usr/bin/script";
const canRun = process.platform === "linux" && existsSync(scriptPath);

test(
  "cli exits cleanly on /quit",
  { skip: canRun ? false : "requires linux util-linux script(1)" },
  () => {
    const result = spawnSync(scriptPath, ["-qec", "node cli.mjs", "/dev/null"], {
      cwd: import.meta.dirname,
      input: "/quit\n",
      timeout: 8000,
      encoding: "utf8",
    });
    assert.equal(result.error, undefined, `spawn failed: ${result.error}`);
    assert.equal(
      result.status,
      0,
      `expected clean exit, got status=${result.status}\n${result.stdout}${result.stderr}`,
    );
  },
);
