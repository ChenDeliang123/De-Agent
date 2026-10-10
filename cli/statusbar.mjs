// 终端提示符与底部状态行文案拼装（零依赖）。
//
// 设计（对齐 pi）：
//   - 提示符极简：只留 `❯ `，输入行不再承载 cwd/分支/模式/会话信息；
//   - 状态行下沉：`~/proj (main) · confirm · f920924d` 由 BottomRegion 常驻在
//     终端最底部（输入框下方）。
//
// 注意：提示符与状态行都会进入按宽度计算的区域渲染，所以宽度一律用
// text.mjs 的可见宽度口径（提示符本身保持纯文本，不带 ANSI）。

import fs from "node:fs";
import path from "node:path";

import { clipToWidth, displayWidth } from "./text.mjs";

export const PROMPT_LABEL = "❯ ";

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

// 权限模式 -> 语义色：workspace 是常规态，confirm 需要留意，full 属危险态。
const MODE_COLORS = { workspace: "muted", confirm: "warning", full: "error" };

export function modeColor(mode) {
  const key = String(mode ?? "").toLowerCase();
  const alias = key === "ask" ? "confirm" : key === "ws" || key === "write" ? "workspace" : key;
  return MODE_COLORS[alias] ?? "muted";
}

function paint(part, text, styler) {
  if (!styler || typeof styler.fg !== "function" || styler.mode === "none") {
    return text;
  }
  const colored = styler.fg(part.color, text);
  return part.bold ? styler.bold(colored) : colored;
}

// 底部状态行：`~/proj (main) · confirm · f920924d · 12% ctx · model`。
// 超宽时从右往左丢字段（宁可少显示，也不截断成半截信息）；只剩一段时才裁剪。
export function formatStatusLine(
  { cwd, branch, mode, sessionId, model, ctxPercent } = {},
  width = 80,
  styler,
) {
  const maxWidth = Math.max(8, Math.floor(Number(width) || 80) - 1);
  const parts = [];
  if (cwd) {
    parts.push({
      text: branch ? `${shortCwd(cwd)} (${branch})` : shortCwd(cwd),
      color: "dim",
    });
  }
  if (mode) {
    parts.push({ text: String(mode), color: modeColor(mode), bold: true });
  }
  if (sessionId) {
    parts.push({ text: String(sessionId).slice(0, 8), color: "dim" });
  }
  if (ctxPercent !== undefined && ctxPercent !== null) {
    parts.push({ text: `${ctxPercent}% ctx`, color: "muted" });
  }
  if (model) {
    parts.push({ text: String(model), color: "muted" });
  }
  if (parts.length === 0) {
    return "";
  }

  const kept = [];
  let used = 0;
  for (const part of parts) {
    const cost = displayWidth(part.text) + (kept.length > 0 ? 3 : 0); // 分隔符 " · "
    if (used + cost > maxWidth) {
      break;
    }
    kept.push(part);
    used += cost;
  }
  if (kept.length === 0) {
    const first = parts[0];
    // clipToWidth 会把省略号加在 maxWidth 之后，这里先让出一列，保证不超宽。
    return paint(first, clipToWidth(first.text, Math.max(1, maxWidth - 1)), styler);
  }
  const separator =
    styler && typeof styler.dim === "function" && styler.mode !== "none" ? styler.dim(" · ") : " · ";
  return kept.map((part) => paint(part, part.text, styler)).join(separator);
}
