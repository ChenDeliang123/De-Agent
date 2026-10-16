// 子 Agent 类指令（category=info）：/agents 打印开关、上限、定义、统计与运行中的后台任务；
// /agents stop <task_id|agent_id|all> 停止后台任务。
// 数据来自后端 `GET /api/v1/agents`；关闭时后端返回 {"enabled":false}。

import { getAgents, stopAgent } from "../session.mjs";

function definitionLine(definition) {
  const name = String(definition?.name ?? "?");
  const model = String(definition?.model ?? "").trim() || "(default)";
  const tools = Array.isArray(definition?.tools) ? definition.tools.map(String) : [];
  const toolList = tools.length ? tools.join(", ") : "(all)";
  return `  ${name.padEnd(12)} model: ${model}  tools: ${toolList}`;
}

// 已运行时长：与 renderer 的子 Agent 生命周期行同一格式（1 分钟内一位小数秒，更长 m/s）。
function formatElapsed(ms) {
  const value = Number(ms);
  if (!Number.isFinite(value) || value < 0) {
    return "";
  }
  if (value < 60_000) {
    return `${(value / 1000).toFixed(1)}s`;
  }
  const minutes = Math.floor(value / 60_000);
  const seconds = Math.round((value % 60_000) / 1000);
  return `${minutes}m ${seconds}s`;
}

// `  bg-3 explorer#7 12.3s <prompt 摘要>`；缺字段自动省略。
function runningLine(item) {
  const parts = [String(item?.task_id ?? "?"), String(item?.agent_id ?? "?")];
  const elapsed = formatElapsed(item?.elapsed_ms);
  if (elapsed) {
    parts.push(elapsed);
  }
  const excerpt = String(item?.prompt_excerpt ?? "").replace(/\s+/g, " ").trim();
  if (excerpt) {
    parts.push(excerpt);
  }
  return `  ${parts.join(" ")}`;
}

// 停止单个后台任务：已结束按后端 message 提示，未知（404）同样按后端 message 报错。
async function stopTask(ctx, id) {
  try {
    const body = await stopAgent(ctx.backend, id);
    if (body?.result === "finished") {
      ctx.print(String(body.message ?? `${id} already finished`));
      return true;
    }
    ctx.print(`stopped ${id}`);
    return true;
  } catch (error) {
    ctx.print(`stop failed: ${error.message}`);
    return false;
  }
}

// stop all：对当前会话 running[] 逐条 stop。
async function stopAll(ctx) {
  const sessionId = ctx.state?.sessionId ?? null;
  if (!sessionId) {
    ctx.print("no active session");
    return;
  }
  let info;
  try {
    info = await getAgents(ctx.backend, sessionId);
  } catch (error) {
    ctx.print(`agents failed: ${error.message}`);
    return;
  }
  const running = Array.isArray(info?.running) ? info.running : [];
  if (running.length === 0) {
    ctx.print("no running background tasks");
    return;
  }
  for (const item of running) {
    const taskId = String(item?.task_id ?? "").trim();
    if (!taskId) {
      continue;
    }
    await stopTask(ctx, taskId);
  }
}

export const agentCommands = [
  {
    name: "agents",
    aliases: [],
    description: "Show sub-agent settings, definitions, stats, and background tasks",
    argHint: "[stop <task_id|agent_id|all>]",
    category: "info",
    handler: async (ctx, arg) => {
      const [sub, id] = String(arg ?? "").trim().split(/\s+/).filter(Boolean);
      if (sub === "stop") {
        if (!id) {
          ctx.print("usage: /agents stop <task_id|agent_id|all>");
          return;
        }
        if (id === "all") {
          await stopAll(ctx);
          return;
        }
        await stopTask(ctx, id);
        return;
      }
      let info;
      try {
        // 有会话时只关心该会话的运行中/待补打任务。
        info = await getAgents(ctx.backend, ctx.state?.sessionId ?? undefined);
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
      const maxBackground = info.max_background ?? "?";
      ctx.print(
        `sub-agents enabled: max_depth=${maxDepth} max_concurrent=${maxConcurrent} max_background=${maxBackground}`,
      );
      const stats = info.stats ?? {};
      const runs = stats.runs ?? 0;
      const failures = stats.failures ?? 0;
      const cancelled = stats.cancelled ?? 0;
      const totalMs = stats.total_ms ?? 0;
      ctx.print(
        `stats: runs=${runs} failures=${failures} cancelled=${cancelled} total_ms=${totalMs}`,
      );
      // 用量归集（M3）：子 Agent 的 token 之和，与父会话 usage 同一口径。
      const prompt = Number(stats.prompt_tokens ?? 0);
      const completion = Number(stats.completion_tokens ?? 0);
      const cacheHit = Number(stats.cache_hit_tokens ?? 0);
      const total = (Number.isFinite(prompt) ? prompt : 0) + (Number.isFinite(completion) ? completion : 0);
      if (total > 0) {
        const avg = runs > 0 ? Math.round(total / runs) : total;
        ctx.print(
          `tokens: prompt=${prompt} completion=${completion} cache_hit=${cacheHit} total=${total} (avg ${avg}/run)`,
        );
      }
      // 运行中的后台任务（M4c）：id / 类型 / 已运行时长 / prompt 摘要。
      const running = Array.isArray(info.running) ? info.running : [];
      if (running.length === 0) {
        ctx.print("running: none");
      } else {
        ctx.print("running:");
        for (const item of running) {
          ctx.print(runningLine(item));
        }
      }
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
