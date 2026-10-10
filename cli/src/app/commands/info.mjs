// 信息与权限类指令（category=info）：/status /tools /cwd /permissions，
// 以及权限模式助手（模式别名、展示、应用）。

import fs from "node:fs";
import path from "node:path";

import {
  effectivePermission,
  health,
  initProjectPermissions,
  listPermissions,
  listSessions,
  listTools,
  resetPermission,
  setPermission,
} from "../session.mjs";

const PERMISSION_MODES = [
  {
    value: "confirm",
    label: "confirm",
    description: "Ask before every file write or command",
  },
  {
    value: "workspace",
    label: "workspace",
    description: "Write inside the workspace; ask before commands",
  },
  {
    value: "full",
    label: "full",
    description: "Unrestricted file and shell access (danger)",
  },
];

const PERMISSION_ALIASES = {
  confirm: "confirm",
  ask: "confirm",
  workspace: "workspace",
  ws: "workspace",
  write: "workspace",
  full: "full",
  danger: "full",
};

function normalizePermissionMode(text) {
  return PERMISSION_ALIASES[String(text).toLowerCase()] ?? null;
}

function printPermissionInfo(ctx, info) {
  const write =
    info.mode === "full" ? "anywhere" : info.mode === "confirm" ? "ask" : "inside workspace";
  const command = info.mode === "full" ? "yes" : "ask";
  ctx.print(`cwd:       ${info.path}`);
  ctx.print(`mode:      ${info.mode} (${info.source})`);
  ctx.print("  read:    yes");
  ctx.print(`  write:   ${write}`);
  ctx.print(`  command: ${command}`);
}

async function applyPermissionMode(ctx, cwd, modeText) {
  const mode = normalizePermissionMode(modeText);
  if (!mode) {
    ctx.print(`unknown mode: ${modeText}; use confirm | workspace | full`);
    return;
  }
  if (mode === "full") {
    const message =
      "FULL ACCESS gives the model unrestricted file and shell access under\n" +
      `  ${cwd}\n` +
      "(it can read/write anywhere and run any command). Enable?";
    const ok = typeof ctx.confirm === "function" ? await ctx.confirm(message) : false;
    if (!ok) {
      ctx.print("cancelled");
      return;
    }
  }
  try {
    const result = await setPermission(ctx.backend, cwd, mode);
    const warn = result.mode === "full" ? "  ⚠ FULL ACCESS" : "";
    ctx.print(`permission for ${cwd}: ${result.mode}${warn}`);
    ctx.log("info", "cli", `event=permission_set path=${cwd} mode=${result.mode}`);
    if (typeof ctx.refreshPromptState === "function") {
      await ctx.refreshPromptState();
    }
  } catch (error) {
    ctx.print(`permission change failed: ${error.message}`);
  }
}

export const infoCommands = [
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
      ctx.state.cwdExplicit = true;
      ctx.print(`cwd: ${resolved}`);
      if (typeof ctx.refreshPromptState === "function") {
        await ctx.refreshPromptState();
      }
      if (typeof ctx.promptProjectTrust === "function") {
        await ctx.promptProjectTrust();
      }
    },
  },
  {
    name: "permissions",
    aliases: ["perms", "permission"],
    description: "Show or change the permission mode for the current directory",
    argHint: "[confirm|workspace|full|show|list|reset|init]",
    category: "local",
    handler: async (ctx, arg) => {
      const cwd = ctx.state.cwd ?? process.cwd();
      const token = (arg ?? "").trim().toLowerCase();

      if (token === "list") {
        try {
          const data = await listPermissions(ctx.backend);
          ctx.print(`default: ${data.default_mode}`);
          const paths = Array.isArray(data.paths) ? data.paths : [];
          if (paths.length === 0) {
            ctx.print("(no explicit permissions)");
            return;
          }
          for (const entry of paths) {
            const warn = entry.mode === "full" ? "  ⚠" : "";
            ctx.print(`  ${entry.path}  ${entry.mode}${warn}`);
          }
        } catch (error) {
          ctx.print(`permission list failed: ${error.message}`);
        }
        return;
      }

      if (token === "reset") {
        try {
          await resetPermission(ctx.backend, cwd);
          const info = await effectivePermission(ctx.backend, cwd);
          ctx.print(
            `cleared explicit permission for ${cwd} (now ${info.source}: ${info.mode})`,
          );
          if (typeof ctx.refreshPromptState === "function") {
            await ctx.refreshPromptState();
          }
        } catch (error) {
          ctx.print(`permission reset failed: ${error.message}`);
        }
        return;
      }

      if (token === "init") {
        try {
          const result = await initProjectPermissions(ctx.backend, cwd, false);
          const created =
            Array.isArray(result.created) && result.created.length > 0
              ? result.created.join(", ")
              : "already present";
          ctx.print(`initialized ${result.root}/.de (${created})`);
        } catch (error) {
          ctx.print(`init failed: ${error.message}`);
        }
        return;
      }

      if (token && token !== "show") {
        await applyPermissionMode(ctx, cwd, token);
        return;
      }

      let info;
      try {
        info = await effectivePermission(ctx.backend, cwd);
      } catch (error) {
        ctx.print(`permission request failed: ${error.message}`);
        return;
      }
      if (token === "show" || typeof ctx.select !== "function") {
        printPermissionInfo(ctx, info);
        return;
      }
      // 交互式选择：当前模式置顶并标注 (current)。
      const ordered = [
        ...PERMISSION_MODES.filter((mode) => mode.value === info.mode),
        ...PERMISSION_MODES.filter((mode) => mode.value !== info.mode),
      ];
      const index = await ctx.select(
        ordered.map((mode) => ({
          label: mode.label,
          description: `${mode.description}${mode.value === info.mode ? "  (current)" : ""}`,
        })),
        `Select permission for ${cwd} (↑/↓ move, Enter apply, Esc cancel):`,
      );
      if (index === null || index === undefined || !ordered[index]) {
        return;
      }
      await applyPermissionMode(ctx, cwd, ordered[index].value);
    },
  },
];
