// 子 Agent 类指令（category=info）：/agents 打印开关、上限、定义、统计与运行中的后台任务；
// /agents stop <task_id|agent_id|all> 停止后台任务；/agents steer <id> <message> 纠偏。
// 数据来自后端 `GET /api/v1/agents`；关闭时后端返回 {"enabled":false}。

import { getAgents, steerAgent, stopAgent } from "../session.mjs";
import { clipToWidth } from "../../ui/text.mjs";

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

// `  bg-3 explorer#7 12.3s <prompt 摘要> (steerable)`；缺字段自动省略。
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
  // 运行中的后台任务可以用 /agents steer <id> <message> 纠偏。
  return `  ${parts.join(" ")} (steerable)`;
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

// steer：给运行中的后台任务发纠偏消息。409/404/400 等都只打印一行，不抛穿。
async function steerTask(ctx, id, text) {
  try {
    const body = await steerAgent(ctx.backend, id, text, ctx.state?.sessionId ?? undefined);
    if (body?.result === "accepted") {
      ctx.print(`steered ${id}`);
      return true;
    }
    ctx.print(String(body?.message ?? `steered ${id}`));
    return true;
  } catch (error) {
    ctx.print(`steer failed: ${error.message}`);
    return false;
  }
}

// `/agents show [id]`：列出/打印当前回合的前台子 Agent transcript。
// transcript 由 CLI 渲染器在内存中保留，后端没有对应接口。
function showTranscript(ctx, id) {
  const renderer = ctx.renderer;
  if (!renderer || typeof renderer.subagentTranscriptIds !== "function") {
    ctx.print("no sub-agent transcripts available");
    return;
  }
  const ids = renderer.subagentTranscriptIds();
  if (!id) {
    if (ids.length === 0) {
      ctx.print("no sub-agent transcripts for this turn");
      return;
    }
    ctx.print("available transcripts:");
    for (const agent of ids) {
      ctx.print(`  ${agent}`);
    }
    ctx.print("usage: /agents show <agent_id>");
    return;
  }
  if (id.startsWith("bg-")) {
    ctx.print(
      `${id} is a background task; transcripts are only kept for foreground sub-agents. Use /agents or task_result.`,
    );
    return;
  }
  const transcript = renderer.subagentTranscript(id);
  if (!transcript) {
    const available = ids.length > 0 ? ` (available: ${ids.join(", ")})` : "";
    ctx.print(`no transcript for ${id}${available}`);
    return;
  }
  const meta =
    transcript.dropped > 0
      ? `last ${transcript.lines.length} lines, ${transcript.dropped} earlier lines dropped`
      : `${transcript.lines.length} lines`;
  ctx.print(`${id} transcript (${meta})`);
  const columns = Math.max(
    20,
    Number(ctx.columns) || Number(process.stdout?.columns) || 80,
  );
  for (const line of transcript.lines) {
    // 2 格缩进 + 可能的省略号：裁剪上限留 3 列，保证整行不超过终端宽度。
    ctx.print(`  ${clipToWidth(line, Math.max(10, columns - 3))}`);
  }
}

export const agentCommands = [
  {
    name: "agents",
    aliases: [],
    description: "Show sub-agent settings, stats, transcripts, and manage background tasks",
    argHint: "[stop <id|all> | steer <id> <message> | show [id]]",
    category: "info",
    handler: async (ctx, arg) => {
      const [sub, id, ...rest] = String(arg ?? "").trim().split(/\s+/).filter(Boolean);
      if (sub === "show") {
        showTranscript(ctx, id);
        return;
      }
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
      if (sub === "steer") {
        const text = rest.join(" ").trim();
        if (!id || !text) {
          ctx.print("usage: /agents steer <task_id|agent_id> <message>");
          return;
        }
        await steerTask(ctx, id, text);
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
