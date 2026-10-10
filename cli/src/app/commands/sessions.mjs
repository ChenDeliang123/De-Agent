// 会话类指令（category=session）：/new /resume /session /name /clone /fork /delete。
// 也承载会话流程助手（恢复 cwd、打开/克隆会话）供 index.mjs 再导出。

import {
  cloneSession,
  deleteSession,
  listSessions,
  renameSession,
  sessionStats,
} from "../session.mjs";

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

export async function restoreSessionCwd(ctx) {
  if (!ctx.state.sessionId || ctx.state.cwdExplicit) {
    return null;
  }
  try {
    const stats = await sessionStats(ctx.backend, ctx.state.sessionId);
    if (typeof stats.cwd === "string" && stats.cwd) {
      ctx.state.cwd = stats.cwd;
      ctx.log(
        "info",
        "cli",
        `event=session_cwd_restored session=${ctx.state.sessionId} cwd=${stats.cwd}`,
      );
      return stats.cwd;
    }
  } catch {
    // 旧后端或不带 cwd 的会话不影响打开流程。
  }
  return null;
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
  const restoredCwd = await restoreSessionCwd(ctx);
  if (restoredCwd) {
    ctx.print(`cwd: ${restoredCwd}`);
  }
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
    await ctx.renderHistory(ctx.backend, ctx.state.sessionId);
    ctx.log("info", "cli", `event=clone_session session=${ctx.state.sessionId}`);
  } catch (error) {
    ctx.print(`clone failed: ${error.message}`);
  }
}

export const sessionCommands = [
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
        const backendCwd =
          typeof stats.cwd === "string" && stats.cwd ? stats.cwd : ctx.state.cwd;
        if (!ctx.state.cwdExplicit) {
          ctx.state.cwd = backendCwd;
        }
        ctx.print(`cwd:        ${backendCwd}`);
        if (Array.isArray(stats.instructions)) {
          if (stats.instructions.length === 0) {
            ctx.print("instructions: (none)");
          } else {
            ctx.print(`instructions: ${stats.instructions.length} file(s)`);
            for (const file of stats.instructions) {
              const suffix = file.truncated ? " (truncated)" : "";
              ctx.print(`  - ${file.path}${suffix}`);
            }
          }
        }
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
];
