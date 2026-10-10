// 上下文类指令（category=context）：/compact。

import { compactSession } from "../session.mjs";

export const contextCommands = [
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
        ctx.renderer?.wait("compacting");
        await compactSession(ctx.backend, ctx.state.sessionId, ctx.state.cwd);
        ctx.renderer?.stopWaiting();
        ctx.print("context compacted");
        ctx.log("info", "cli", `event=compact session=${ctx.state.sessionId}`);
      } catch (error) {
        ctx.renderer?.stopWaiting();
        ctx.print(`compact failed: ${error.message}`);
        ctx.log("error", "cli", `event=compact_failed message=${error.message}`);
      }
    },
  },
];
