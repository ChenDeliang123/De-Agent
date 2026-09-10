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
// handler 的 ctx 由 cli.mjs 注入，约定字段：
//   { backend, state, print, log, clearInputLine, clearScreen, renderHistory, commands }
// 其中 state 至少包含 { sessionId, shouldExit }。

import { compactSession, listSessions } from "./session.mjs";
import { renderHotkeys } from "./editor.mjs";

const CATEGORY_ORDER = ["session", "context", "info", "local"];

const CATEGORY_TITLES = {
  session: "Session",
  context: "Context",
  info: "Info",
  local: "Local",
};

async function showSessions(ctx) {
  const sessions = await listSessions(ctx.backend);
  if (sessions.length === 0) {
    ctx.print("(no sessions yet)");
    return [];
  }
  sessions.forEach((session, index) => {
    const id = typeof session === "string" ? session : session.id;
    const title =
      typeof session === "string" ? `session ${index + 1}` : session.title || "untitled";
    ctx.print(`  ${index + 1}. ${title}  (${id})`);
  });
  return sessions;
}

export const commands = [
  {
    name: "new",
    aliases: [],
    description: "Start a new session",
    argHint: "",
    category: "session",
    handler: async (ctx) => {
      ctx.state.sessionId = null;
      ctx.print("new session");
      ctx.log("info", "cli", "event=new_session");
    },
  },
  {
    name: "sessions",
    aliases: [],
    description: "List backend sessions",
    argHint: "",
    category: "session",
    handler: async (ctx) => {
      await showSessions(ctx);
    },
  },
  {
    name: "open",
    aliases: [],
    description: "Resume a session by id or index",
    argHint: "<id|index>",
    category: "session",
    handler: async (ctx, arg) => {
      const all = await listSessions(ctx.backend);
      let target = (arg ?? "").trim();
      if (!target && all.length > 0) {
        target = String(all.length);
      }
      if (!target) {
        ctx.print("no sessions to open");
        return;
      }
      const numeric = Number(target);
      const entry =
        Number.isInteger(numeric) && numeric >= 1 && numeric <= all.length
          ? all[numeric - 1]
          : all.find(
              (session) => (typeof session === "string" ? session : session.id) === target,
            );
      if (!entry) {
        ctx.print(`session not found: ${target}`);
        return;
      }
      ctx.state.sessionId = typeof entry === "string" ? entry : entry.id;
      ctx.clearInputLine();
      await ctx.renderHistory(ctx.backend, ctx.state.sessionId);
      ctx.log("info", "cli", `event=open_session session=${ctx.state.sessionId}`);
    },
  },
  {
    name: "compact",
    aliases: [],
    description: "Summarize and shrink the current session context",
    argHint: "",
    category: "context",
    handler: async (ctx) => {
      if (!ctx.state.sessionId) {
        ctx.print("no active session to compact");
        return;
      }
      try {
        await compactSession(ctx.backend, ctx.state.sessionId);
        ctx.print("context compacted");
        ctx.log("info", "cli", `event=compact session=${ctx.state.sessionId}`);
      } catch (error) {
        ctx.print(`compact failed: ${error.message}`);
        ctx.log("error", "cli", `event=compact_failed message=${error.message}`);
      }
    },
  },
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
  {
    name: "clear",
    aliases: [],
    description: "Clear the screen",
    argHint: "",
    category: "local",
    handler: async (ctx) => {
      ctx.clearScreen();
    },
  },
  {
    name: "quit",
    aliases: ["exit"],
    description: "Exit",
    argHint: "",
    category: "local",
    handler: async (ctx) => {
      ctx.state.shouldExit = true;
    },
  },
];

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
  lines.push("type / to list commands, Tab to complete it, /hotkeys for keybindings");
  return lines.join("\n");
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
  return true;
}
