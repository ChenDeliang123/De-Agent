// 终端状态栏/提示符文案拼装（纯文本，零依赖）。
//
// 注意：输入提示符走 LineEditor，宽度按 displayWidth 计算；
// 为避免光标定位出错，提示符本身保持纯文本（不带 ANSI）。

import fs from "node:fs";
import path from "node:path";

import { clipToWidth } from "./editor.mjs";

export function shortCwd(cwd, home = process.env.HOME) {
  if (!cwd) {
    return "";
  }
  if (!home) {
    return cwd;
  }
  if (cwd === home) {
    return "~";
  }
  if (cwd.startsWith(`${home}/`)) {
    return `~${cwd.slice(home.length)}`;
  }
  return cwd;
}

// 从 cwd 向上找 .git/HEAD，解析当前分支（detached 时返回短 hash）。
export function gitBranch(cwd) {
  let dir = cwd;
  if (!dir) {
    return "";
  }
  for (let i = 0; i < 30; i += 1) {
    const head = path.join(dir, ".git", "HEAD");
    try {
      const content = fs.readFileSync(head, "utf8").trim();
      const match = content.match(/^ref:\s+refs\/heads\/(.+)$/);
      if (match) {
        return match[1];
      }
      if (/^[0-9a-f]{7,}$/i.test(content)) {
        return content.slice(0, 7);
      }
      return "";
    } catch {
      // 继续向上
    }
    const parent = path.dirname(dir);
    if (parent === dir) {
      return "";
    }
    dir = parent;
  }
  return "";
}

// 输入提示符：`~/proj (main) workspace ❯ `（纯文本）。
export function formatPromptLabel({ cwd, branch, mode, sessionId } = {}) {
  const parts = [];
  if (cwd) {
    parts.push(shortCwd(cwd));
  }
  if (branch) {
    parts.push(`(${branch})`);
  }
  if (mode) {
    parts.push(mode);
  }
  if (sessionId) {
    parts.push(sessionId.slice(0, 8));
  }
  const prefix = parts.length > 0 ? `${parts.join(" ")} ` : "";
  return `${prefix}❯ `;
}

// 一行状态摘要：`~/proj (main) · workspace · 12% ctx · model`，按宽度截断。
export function formatStatusLine({ cwd, branch, mode, ctxPercent, model } = {}, width = 80) {
  const parts = [];
  if (cwd) {
    parts.push(branch ? `${shortCwd(cwd)} (${branch})` : shortCwd(cwd));
  }
  if (mode) {
    parts.push(mode);
  }
  if (ctxPercent !== undefined && ctxPercent !== null) {
    parts.push(`${ctxPercent}% ctx`);
  }
  if (model) {
    parts.push(model);
  }
  return clipToWidth(parts.join(" · "), Math.max(10, width - 1));
}
