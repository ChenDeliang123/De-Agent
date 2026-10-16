// 指令注册表：slash 指令的唯一事实来源。
// help、输入补全、命令分发都从这里派生，避免实现与帮助文本漂移。
//
// 每条指令形如：
//   {
//     name: "open",                 // 主名（不含前导 /）
//     aliases: ["resume"],          // 可选别名
//     description: "Resume a session",
//     argHint: "<id>",              // 可选参数提示
//     category: "session",          // session | context | info | local
//     handler: async (ctx, arg) => {} // 执行体
//   }
//
// 指令实体按类别拆分在同目录的 sessions/context/info/local 模块里；
// 本文件负责拼装顺序、注册表查询 API 与帮助文本（含 /help、/hotkeys）。
//
// handler 的 ctx 由 cli.mjs 注入，约定字段：
//   { backend, state, print, log, clearScreen, updateFooter?, renderHistory,
//     commands, selectSession?, confirm? }
// 其中 state 至少包含 { sessionId, cwd, shouldExit, pendingName? }。

import { renderHotkeys } from "../../ui/editor.mjs";
import { agentCommands } from "./agents.mjs";
import { contextCommands } from "./context.mjs";
import { infoCommands } from "./info.mjs";
import { clearCommand, quitCommand, themeCommand } from "./local.mjs";
import { sessionCommands } from "./sessions.mjs";

export { restoreSessionCwd } from "./sessions.mjs";

// /help 与 /hotkeys 属于注册表自身的说明，放在这里可避免 help 模块反向依赖注册表。
const helpCommands = [
  {
    name: "help",
    aliases: [],
    description: "Show this help",
    argHint: "",
    category: "info",
    handler: async (ctx) => {
      ctx.print(renderHelp(ctx.commands ?? commands));
    },
  },
  {
    name: "hotkeys",
    aliases: [],
    description: "Show keyboard shortcuts",
    argHint: "",
    category: "info",
    handler: async (ctx) => {
      ctx.print(renderHotkeys());
    },
  },
];

// 顺序即候选列表顺序（/help 内部按 category 分组渲染）。
export const commands = [
  ...sessionCommands,
  ...contextCommands,
  ...infoCommands, // /status /tools /cwd /permissions
  ...agentCommands, // /agents
  themeCommand,
  ...helpCommands,
  clearCommand,
  quitCommand,
];

const CATEGORY_ORDER = ["session", "context", "info", "local"];

const CATEGORY_TITLES = {
  session: "Session",
  context: "Context",
  info: "Info",
  local: "Local",
};

// 帮助文本由注册表派生：新增指令后 /help 不会漂移。
export function renderHelp(registry = commands) {
  const lines = ["commands:"];
  for (const category of CATEGORY_ORDER) {
    const group = registry.filter((command) => command.category === category);
    if (group.length === 0) {
      continue;
    }
    lines.push(`  ${CATEGORY_TITLES[category] ?? category}:`);
    for (const command of group) {
      const arg = command.argHint ? ` ${command.argHint}` : "";
      const aliases =
        (command.aliases ?? []).length > 0
          ? ` (${command.aliases.map((alias) => `/${alias}`).join(", ")})`
          : "";
      const usage = `    /${command.name}${arg}${aliases}`;
      lines.push(`${usage.padEnd(34)}${command.description}`);
    }
  }
  lines.push("");
  lines.push(
    "type / to list commands, Tab to complete it, Ctrl+G for external editor, /hotkeys for keybindings",
  );
  return lines.join("\n");
}

export function findCommand(name, registry = commands) {
  const target = String(name).toLowerCase();
  return (
    registry.find(
      (command) =>
        command.name === target || (command.aliases ?? []).includes(target),
    ) ?? null
  );
}

export function completeCommands(prefix, registry = commands) {
  const target = String(prefix).toLowerCase();
  return registry.filter((command) => {
    const names = [command.name, ...(command.aliases ?? [])];
    return names.some((name) => name.startsWith(target));
  });
}

function levenshtein(a, b) {
  const rows = a.length + 1;
  const cols = b.length + 1;
  let previous = Array.from({ length: cols }, (_, index) => index);
  for (let i = 1; i < rows; i += 1) {
    const current = [i];
    for (let j = 1; j < cols; j += 1) {
      const cost = a[i - 1] === b[j - 1] ? 0 : 1;
      current[j] = Math.min(previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost);
    }
    previous = current;
  }
  return previous[cols - 1];
}

// 为拼错的指令名给一个最接近的建议；找不到就返回 null。
export function suggestCommand(name, registry = commands) {
  const target = String(name).toLowerCase();
  let best = null;
  let bestDistance = Infinity;
  for (const command of registry) {
    for (const candidate of [command.name, ...(command.aliases ?? [])]) {
      const distance = levenshtein(target, candidate);
      if (distance < bestDistance) {
        bestDistance = distance;
        best = command.name;
      }
    }
  }
  const threshold = target.length <= 3 ? 1 : 2;
  return bestDistance <= threshold ? best : null;
}

// 解析并执行一条以 "/" 开头的指令。返回是否命中指令。
export async function runCommand(ctx, text) {
  const registry = ctx.commands ?? commands;
  const trimmed = text.trim();
  const withoutSlash = trimmed.startsWith("/") ? trimmed.slice(1) : trimmed;
  const [rawName, ...rest] = withoutSlash.split(/\s+/);
  const command = findCommand(rawName, registry);
  if (!command) {
    const suggestion = suggestCommand(rawName, registry);
    ctx.print(
      suggestion
        ? `unknown command: /${rawName}; did you mean /${suggestion}?`
        : `unknown command: /${rawName}; use /help`,
    );
    return false;
  }
  await command.handler(ctx, rest.join(" "));
  // 命令可能改了 cwd / 会话 / 权限模式：统一刷新最底部状态行。
  ctx.updateFooter?.();
  return true;
}
