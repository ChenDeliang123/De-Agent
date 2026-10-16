// 子 Agent 类指令（category=info）：/agents 打印开关、上限、定义列表与运行统计。
// 数据来自后端 `GET /api/v1/agents`；关闭时后端返回 {"enabled":false}。

import { getAgents } from "../session.mjs";

function definitionLine(definition) {
  const name = String(definition?.name ?? "?");
  const model = String(definition?.model ?? "").trim() || "(default)";
  const tools = Array.isArray(definition?.tools) ? definition.tools.map(String) : [];
  const toolList = tools.length ? tools.join(", ") : "(all)";
  return `  ${name.padEnd(12)} model: ${model}  tools: ${toolList}`;
}

export const agentCommands = [
  {
    name: "agents",
    aliases: [],
    description: "Show sub-agent settings, definitions, and stats",
    argHint: "",
    category: "info",
    handler: async (ctx) => {
      let info;
      try {
        info = await getAgents(ctx.backend);
      } catch (error) {
        ctx.print(`agents failed: ${error.message}`);
        return;
      }
      if (!info || info.enabled !== true) {
        ctx.print("sub-agents disabled");
        return;
      }
      const maxDepth = info.max_depth ?? "?";
      const maxConcurrent = info.max_concurrent ?? "?";
      ctx.print(`sub-agents enabled: max_depth=${maxDepth} max_concurrent=${maxConcurrent}`);
      const stats = info.stats ?? {};
      const runs = stats.runs ?? 0;
      const failures = stats.failures ?? 0;
      const cancelled = stats.cancelled ?? 0;
      const totalMs = stats.total_ms ?? 0;
      ctx.print(
        `stats: runs=${runs} failures=${failures} cancelled=${cancelled} total_ms=${totalMs}`,
      );
      const definitions = Array.isArray(info.definitions) ? info.definitions : [];
      if (definitions.length === 0) {
        ctx.print("definitions: (none)");
        return;
      }
      ctx.print("definitions:");
      for (const definition of definitions) {
        ctx.print(definitionLine(definition));
      }
    },
  },
];
