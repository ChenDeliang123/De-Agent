// 外部编辑器支持（Ctrl+G）。
//
// 语义对齐 pi：把当前输入写入临时目录下的 prompt.md，交给外部编辑器编辑，
// 退出后读回内容。编辑器命令解析优先级：
//   --editor > $AIAGENT_EDITOR > $VISUAL > $EDITOR > 平台默认。
//
// 注意：不要用 spawnSync。Windows 下同步 child_process 调用会在父进程暂停
// stdin 后仍保持控制台输入读取，和 vim/nvim 抢缓冲区，参考 pi 的实现注释。

import { spawn } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

export function stripBom(text) {
  return text.startsWith("\uFEFF") ? text.slice(1) : text;
}

export function resolveEditorCommand({ flag, env = process.env, platform = process.platform } = {}) {
  if (typeof flag === "string" && flag.trim() !== "") {
    return flag.trim();
  }
  if (typeof env.AIAGENT_EDITOR === "string" && env.AIAGENT_EDITOR.trim() !== "") {
    return env.AIAGENT_EDITOR.trim();
  }
  if (typeof env.VISUAL === "string" && env.VISUAL.trim() !== "") {
    return env.VISUAL.trim();
  }
  if (typeof env.EDITOR === "string" && env.EDITOR.trim() !== "") {
    return env.EDITOR.trim();
  }
  return platform === "win32" ? "notepad" : "nano";
}

// 返回 { status: "complete", content } 或 { status: "failed" }。
export async function editInExternalEditor({ command, content, out = process.stdout }) {
  const directory = mkdtempSync(join(tmpdir(), "aiagent-editor-"));
  const filePath = join(directory, "prompt.md");
  try {
    writeFileSync(filePath, content, "utf-8");
    out.write(
      `Launching external editor: ${command}\n` +
        "ai-agent will resume when the editor exits.\n",
    );

    const [editor, ...editorArgs] = command.trim().split(/\s+/);
    const exitCode = await new Promise((resolve) => {
      const child = spawn(editor, [...editorArgs, filePath], {
        stdio: "inherit",
        shell: process.platform === "win32",
      });
      child.on("error", () => resolve(null));
      child.on("close", (code) => resolve(code));
    });

    if (exitCode !== 0) {
      return { status: "failed" };
    }

    return {
      status: "complete",
      content: stripBom(readFileSync(filePath, "utf-8")).replace(/\n$/, ""),
    };
  } finally {
    try {
      rmSync(directory, { recursive: true, force: true });
    } catch {
      // 清理失败不影响结果。
    }
  }
}
