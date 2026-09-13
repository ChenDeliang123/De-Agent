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
//   { backend, state, print, log, clearInputLine, clearScreen, renderHistory,
//     commands, selectSession?, confirm? }
// 其中 state 至少包含 { sessionId, cwd, shouldExit, pendingName? }。

import fs from "node:fs";
import path from "node:path";

import {
  cloneSession,
  compactSession,
  deleteSession,
  health,
  listSessions,
  listTools,
  renameSession,
  sessionStats,
} from "./session.mjs";
import { renderHotkeys } from "./editor.mjs";

const CATEGORY_ORDER = ["session", "context", "info", "local"];

const CATEGORY_TITLES = {
  session: "Session",
  context: "Context",
  info: "Info",
  local: "Local",
};

function sessionId(entry) {
  return typeof entry === "string" ? entry : entry.id;
}

function sessionTitle(entry, index) {
  if (typeof entry === "string") {
    return `session ${index + 1}`;
  }
  return entry.title || "untitled";
}

function listSessionsFormatted(ctx, all) {
  if (all.length === 0) {
    ctx.print("(no sessions yet)");
    return;
  }
  all.forEach((entry, index) => {
    ctx.print(`  ${index + 1}. ${sessionTitle(entry, index)}  (${sessionId(entry)})`);
  });
}

async function openSession(ctx, all, target) {
  const numeric = Number(target);
  const entry =
    Number.isInteger(numeric) && numeric >= 1 && numeric <= all.length
      ? all[numeric - 1]
      : all.find((session) => sessionId(session) === target);
  if (!entry) {
    ctx.print(`session not found: ${target}`);
    return false;
  }
  ctx.state.sessionId = sessionId(entry);
  ctx.clearInputLine();
  await ctx.renderHistory(ctx.backend, ctx.state.sessionId);
  ctx.log("info", "cli", `event=open_session session=${ctx.state.sessionId}`);
  return true;
}

async function cloneCurrentSession(ctx) {
  if (!ctx.state.sessionId) {
    ctx.print("no active session to clone");
    return;
  }
  try {
    const result = await cloneSession(ctx.backend, ctx.state.sessionId);
    ctx.state.sessionId = result.session_id;
    ctx.print(`cloned to session ${result.session_id}`);
    ctx.clearInputLine();
    await ctx.renderHistory(ctx.backend, ctx.state.sessionId);
    ctx.log("info", "cli", `event=clone_session session=${ctx.state.sessionId}`);
  } catch (error) {
    ctx.print(`clone failed: ${error.message}`);
  }
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
    name: "resume",
    aliases: ["sessions", "open"],
    description: "Resume a previous session",
    argHint: "[id|index]",
    category: "session",
    handler: async (ctx, arg) => {
      const all = await listSessions(ctx.backend);
      if (all.length === 0) {
        ctx.print("(no sessions yet)");
        return;
      }
      const target = (arg ?? "").trim();
      if (target) {
        await openSession(ctx, all, target);
        return;
      }
      if (typeof ctx.selectSession === "function") {
        const index = await ctx.selectSession(all);
        if (index === null || index === undefined || !all[index]) {
          return;
        }
        await openSession(ctx, all, sessionId(all[index]));
        return;
      }
      listSessionsFormatted(ctx, all);
      ctx.print("use /resume <index|id> to resume one");
    },
  },
  {
    name: "session",
    aliases: [],
    description: "Show current session info and stats",
    argHint: "",
    category: "session",
    handler: async (ctx) => {
      if (!ctx.state.sessionId) {
        ctx.print("no active session");
        return;
      }
      try {
        const stats = await sessionStats(ctx.backend, ctx.state.sessionId);
        ctx.print(`session:    ${stats.id}`);
        ctx.print(
          `messages:   ${stats.messages} (user ${stats.user_messages}, assistant ${stats.assistant_messages})`,
        );
        ctx.print(`characters: ${stats.characters}`);
        ctx.print(`tasks:      ${stats.tasks}`);
        if (stats.compaction) {
          ctx.print(
            `compaction: ${stats.compaction.tokens_before} → ${stats.compaction.tokens_after} tokens`,
          );
        } else {
          ctx.print("compaction: (none)");
        }
        ctx.print(`cwd:        ${ctx.state.cwd}`);
      } catch (error) {
        ctx.print(`session info failed: ${error.message}`);
      }
    },
  },
  {
    name: "name",
    aliases: [],
    description: "Set the current session display name",
    argHint: "<title>",
    category: "session",
    handler: async (ctx, arg) => {
      if (!ctx.state.sessionId) {
        ctx.print("no active session");
        return;
      }
      const title = (arg ?? "").trim();
      if (!title) {
        ctx.print("usage: /name <title>");
        return;
      }
      try {
        const result = await renameSession(ctx.backend, ctx.state.sessionId, title);
        ctx.print(`renamed: ${result.title ?? title}`);
        ctx.log("info", "cli", `event=rename_session session=${ctx.state.sessionId}`);
      } catch (error) {
        ctx.print(`rename failed: ${error.message}`);
      }
    },
  },
  {
    name: "clone",
    aliases: [],
    description: "Duplicate the current session into a new one",
    argHint: "",
    category: "session",
    handler: async (ctx) => {
      await cloneCurrentSession(ctx);
    },
  },
  {
    name: "fork",
    aliases: [],
    description: "Fork the current session at its latest point",
    argHint: "",
    category: "session",
    handler: async (ctx) => {
      await cloneCurrentSession(ctx);
    },
  },
  {
    name: "delete",
    aliases: [],
    description: "Delete the current (or given) session",
    argHint: "[id] [--yes]",
    category: "session",
    handler: async (ctx, arg) => {
      const tokens = (arg ?? "")
        .trim()
        .split(/\s+/)
        .filter(Boolean);
      const force = tokens.includes("--yes") || tokens.includes("-y");
      const target = tokens.find((token) => !token.startsWith("-")) ?? ctx.state.sessionId;
      if (!target) {
        ctx.print("no active session to delete");
        return;
      }
      if (!force) {
        if (typeof ctx.confirm !== "function") {
          ctx.print("refusing to delete without confirmation; use /delete <id> --yes");
          return;
        }
        const confirmed = await ctx.confirm(`delete session ${target}?`);
        if (!confirmed) {
          ctx.print("delete cancelled");
          return;
        }
      }
      try {
        await deleteSession(ctx.backend, target);
        if (ctx.state.sessionId === target) {
          ctx.state.sessionId = null;
        }
        ctx.print(`deleted session ${target}`);
        ctx.log("info", "cli", `event=delete_session session=${target}`);
      } catch (error) {
        ctx.print(`delete failed: ${error.message}`);
      }
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
    name: "status",
    aliases: [],
    description: "Show backend status and current session",
    argHint: "",
    category: "info",
    handler: async (ctx) => {
      let healthy = false;
      try {
        await health(ctx.backend);
        healthy = true;
      } catch {
        healthy = false;
      }
      let count = 0;
      try {
        count = (await listSessions(ctx.backend)).length;
      } catch {
        count = 0;
      }
      ctx.print(`backend:  ${healthy ? "ok" : "unreachable"}  ${ctx.backend}`);
      ctx.print(`sessions: ${count}`);
      ctx.print(`session:  ${ctx.state.sessionId ?? "(none)"}`);
      ctx.print(`cwd:      ${ctx.state.cwd}`);
    },
  },
  {
    name: "tools",
    aliases: [],
    description: "List tools available to the agent",
    argHint: "",
    category: "info",
    handler: async (ctx) => {
      try {
        const tools = await listTools(ctx.backend);
        if (tools.length === 0) {
          ctx.print("(no tools)");
          return;
        }
        for (const tool of tools) {
          const description = String(tool.description ?? "").split("\n")[0];
          ctx.print(`  ${String(tool.name).padEnd(18)} ${description}`);
        }
      } catch (error) {
        ctx.print(`tools failed: ${error.message}`);
      }
    },
  },
  {
    name: "cwd",
    aliases: [],
    description: "Show or change the tool working directory",
    argHint: "[dir]",
    category: "local",
    handler: async (ctx, arg) => {
      const target = (arg ?? "").trim();
      if (!target) {
        ctx.print(`cwd: ${ctx.state.cwd}`);
        return;
      }
      const base = ctx.state.cwd ?? process.cwd();
      const resolved = path.resolve(base, target);
      try {
        if (!fs.statSync(resolved).isDirectory()) {
          ctx.print(`not a directory: ${resolved}`);
          return;
        }
      } catch {
        ctx.print(`directory not found: ${resolved}`);
        return;
      }
      ctx.state.cwd = resolved;
      ctx.print(`cwd: ${resolved}`);
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
  lines.push(
    "type / to list commands, Tab to complete it, Ctrl+G for external editor, /hotkeys for keybindings",
  );
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
