import assert from "node:assert/strict";
import http from "node:http";
import test from "node:test";

import { commands, renderHelp, runCommand } from "../../src/app/commands/index.mjs";

function listen(server) {
  return new Promise((resolve) => {
    server.listen(0, "127.0.0.1", () => resolve(server.address().port));
  });
}

function close(server) {
  return new Promise((resolve) => server.close(resolve));
}

// 契约样例：GET /api/v1/agents 开启时的响应体。
const ENABLED = {
  enabled: true,
  max_depth: 1,
  max_concurrent: 3,
  // 后台子 Agent（M4c）：0 表示关闭；running/pending_results 均可选。
  max_background: 2,
  stats: {
    runs: 3,
    failures: 1,
    cancelled: 0,
    total_ms: 12345,
    prompt_tokens: 30000,
    completion_tokens: 3000,
    cache_hit_tokens: 12000,
  },
  running: [
    {
      task_id: "bg-1",
      agent_id: "explorer#7",
      agent_type: "explorer",
      session_id: "s1",
      state: "running",
      elapsed_ms: 12300,
      prompt_excerpt: "scan the cli",
    },
  ],
  definitions: [
    { name: "general", description: "General-purpose helper", model: "", tools: [] },
    {
      name: "explorer",
      description: "Read-only exploration",
      model: "deepseek-v4-flash",
      tools: ["read", "grep", "glob", "ls"],
    },
  ],
};

function makeContext(backend, sessionId = null) {
  const printed = [];
  const ctx = {
    backend,
    state: { sessionId, cwd: "/tmp", shouldExit: false },
    commands,
    print: (line) => printed.push(line),
    log: () => {},
  };
  return { ctx, printed };
}

async function startBackend(status, payload) {
  const requests = [];
  const server = http.createServer((request, response) => {
    requests.push({ method: request.method, url: request.url });
    response.writeHead(status, { "content-type": "application/json" });
    response.end(JSON.stringify(payload));
  });
  const port = await listen(server);
  return { server, requests, url: `http://127.0.0.1:${port}` };
}

// 多路由假后端：记录 method/url/body，由调用方按路由回包。
async function startRoutesBackend(handle) {
  const requests = [];
  const server = http.createServer((request, response) => {
    const chunks = [];
    request.on("data", (chunk) => chunks.push(chunk));
    request.on("end", () => {
      const body = Buffer.concat(chunks).toString("utf8");
      requests.push({ method: request.method, url: request.url, body });
      handle(request, response, body);
    });
  });
  const port = await listen(server);
  return { server, requests, url: `http://127.0.0.1:${port}` };
}

function sendJson(response, status, payload) {
  response.writeHead(status, { "content-type": "application/json" });
  response.end(JSON.stringify(payload));
}

test("/agents prints settings, definitions, and stats", async () => {
  const backend = await startBackend(200, ENABLED);
  try {
    // 有会话时带 session_id：运行中/待补打列表按会话过滤。
    const { ctx, printed } = makeContext(backend.url, "s1");
    assert.equal(await runCommand(ctx, "/agents"), true);
    assert.deepEqual(backend.requests, [{ method: "GET", url: "/api/v1/agents?session_id=s1" }]);
    const text = printed.join("\n");
    assert.ok(
      printed.includes("sub-agents enabled: max_depth=1 max_concurrent=3 max_background=2"),
      `missing limits line: ${text}`,
    );
    assert.ok(
      printed.includes("stats: runs=3 failures=1 cancelled=0 total_ms=12345"),
      `missing stats line: ${text}`,
    );
    // 用量归集（M3）：stats 带 token 时补一行 tokens（含每次均值）。
    assert.ok(
      printed.includes(
        "tokens: prompt=30000 completion=3000 cache_hit=12000 total=33000 (avg 11000/run)",
      ),
      `missing tokens line: ${text}`,
    );
    // 运行中的后台任务（M4c）：`<task_id> <agent_id> <已运行时长> <prompt 摘要> (steerable)`。
    assert.ok(printed.includes("running:"), `missing running header: ${text}`);
    assert.ok(
      printed.includes("  bg-1 explorer#7 12.3s scan the cli (steerable)"),
      `missing running row: ${text}`,
    );
    assert.ok(printed.includes("definitions:"), `missing definitions header: ${text}`);
    const general = printed.find((line) => line.includes("general") && line.includes("model:"));
    assert.ok(
      general.includes("model: (default)") && general.includes("tools: (all)"),
      `general definition should fall back to defaults: ${general}`,
    );
    const explorer = printed.find((line) => line.includes("model: deepseek-v4-flash"));
    assert.ok(
      explorer.includes("model: deepseek-v4-flash") &&
        explorer.includes("tools: read, grep, glob, ls"),
      `explorer definition should list model and tools: ${explorer}`,
    );
  } finally {
    await close(backend.server);
  }
});

test("/agents prints a single line when sub-agents are disabled", async () => {
  const backend = await startBackend(200, { enabled: false });
  try {
    const { ctx, printed } = makeContext(backend.url);
    assert.equal(await runCommand(ctx, "/agents"), true);
    assert.deepEqual(printed, ["sub-agents disabled"]);
  } finally {
    await close(backend.server);
  }
});

test("/agents reports backend failures", async () => {
  const backend = await startBackend(500, { error: "boom" });
  try {
    const { ctx, printed } = makeContext(backend.url);
    await runCommand(ctx, "/agents");
    assert.deepEqual(printed, ["agents failed: failed to load agents: HTTP 500"]);
  } finally {
    await close(backend.server);
  }
});

test("/agents is registered under info and shows up in /help", () => {
  const command = commands.find((entry) => entry.name === "agents");
  assert.ok(command, "missing /agents command");
  assert.equal(command.category, "info");
  // stop/steer 子命令通过参数传入，不新增顶层指令（候选顺序依赖顶层列表稳定）。
  assert.equal(command.argHint, "[stop <id|all> | steer <id> <message>]");
  assert.ok(renderHelp().includes("/agents"), "/help should list /agents");
});

test("/agents prints running: none and falls back when background fields are missing", async () => {
  const payload = { ...ENABLED, max_background: undefined, running: undefined };
  const backend = await startBackend(200, payload);
  try {
    const { ctx, printed } = makeContext(backend.url);
    await runCommand(ctx, "/agents");
    assert.ok(
      printed.includes("sub-agents enabled: max_depth=1 max_concurrent=3 max_background=?"),
      printed.join("\n"),
    );
    assert.ok(printed.includes("running: none"), printed.join("\n"));
  } finally {
    await close(backend.server);
  }
});

test("/agents stop <task_id> requests a background stop", async () => {
  const backend = await startRoutesBackend((request, response) => {
    if (request.url === "/api/v1/agents/bg-3/stop" && request.method === "POST") {
      return sendJson(response, 200, {
        result: "requested",
        message: "stopping bg-3 (explorer#7)",
      });
    }
    if (request.url === "/api/v1/agents/bg-4/stop" && request.method === "POST") {
      return sendJson(response, 200, { result: "finished", message: "bg-4 already finished (done)" });
    }
    if (request.url === "/api/v1/agents/explorer%237/stop" && request.method === "POST") {
      return sendJson(response, 200, { result: "requested", message: "stopping explorer#7" });
    }
    if (request.url === "/api/v1/agents/bg-9/stop" && request.method === "POST") {
      return sendJson(response, 404, { error: "unknown background task: bg-9" });
    }
    return sendJson(response, 500, { error: `unexpected ${request.method} ${request.url}` });
  });
  try {
    const { ctx, printed } = makeContext(backend.url, "s1");
    assert.equal(await runCommand(ctx, "/agents stop bg-3"), true);
    assert.deepEqual(printed, ["stopped bg-3"]);
    assert.deepEqual(backend.requests[0], {
      method: "POST",
      url: "/api/v1/agents/bg-3/stop",
      body: "{}",
    });

    // 已结束：按后端 message 提示，不重复喊 stopped。
    await runCommand(ctx, "/agents stop bg-4");
    assert.equal(printed.at(-1), "bg-4 already finished (done)");

    // agent_id 形态同样接受；`#` 会转义。
    await runCommand(ctx, "/agents stop explorer#7");
    assert.equal(printed.at(-1), "stopped explorer#7");
    assert.equal(backend.requests.at(-1).url, "/api/v1/agents/explorer%237/stop");

    // 未知任务：后端 404 的 error 透出。
    await runCommand(ctx, "/agents stop bg-9");
    assert.equal(printed.at(-1), "stop failed: unknown background task: bg-9");
  } finally {
    await close(backend.server);
  }
});

test("/agents stop all stops every running task of the session", async () => {
  const RUNNING = {
    ...ENABLED,
    running: [
      { task_id: "bg-1", agent_id: "explorer#7", agent_type: "explorer", elapsed_ms: 5000, prompt_excerpt: "a" },
      { task_id: "bg-2", agent_id: "general#2", agent_type: "general", elapsed_ms: 65000, prompt_excerpt: "b" },
    ],
  };
  const stops = [];
  const backend = await startRoutesBackend((request, response) => {
    if (request.url === "/api/v1/agents?session_id=s1" && request.method === "GET") {
      return sendJson(response, 200, RUNNING);
    }
    if (request.url.startsWith("/api/v1/agents/bg-") && request.method === "POST") {
      const id = request.url.split("/")[4];
      stops.push(id);
      return sendJson(response, 200, { result: "requested", message: `stopping ${id}` });
    }
    return sendJson(response, 500, { error: `unexpected ${request.method} ${request.url}` });
  });
  try {
    const { ctx, printed } = makeContext(backend.url, "s1");
    await runCommand(ctx, "/agents stop all");
    assert.deepEqual(printed, ["stopped bg-1", "stopped bg-2"]);
    assert.deepEqual(stops, ["bg-1", "bg-2"]);
    assert.deepEqual(backend.requests[0], {
      method: "GET",
      url: "/api/v1/agents?session_id=s1",
      body: "",
    });
  } finally {
    await close(backend.server);
  }
});

test("/agents stop all handles empty and missing sessions", async () => {
  const backend = await startRoutesBackend((request, response) => {
    if (request.url === "/api/v1/agents?session_id=s1" && request.method === "GET") {
      return sendJson(response, 200, { enabled: true, max_background: 2, running: [] });
    }
    return sendJson(response, 500, { error: `unexpected ${request.method} ${request.url}` });
  });
  try {
    const { ctx, printed } = makeContext(backend.url, "s1");
    await runCommand(ctx, "/agents stop all");
    assert.deepEqual(printed, ["no running background tasks"]);

    // 没有会话时不应打后端（runCommand 仍返回 true，参数被 handler 消费）。
    const bare = makeContext(backend.url);
    assert.equal(await runCommand(bare.ctx, "/agents stop all"), true);
    assert.deepEqual(bare.printed, ["no active session"]);

    // 缺 id：只提示用法。
    await runCommand(ctx, "/agents stop");
    assert.equal(printed.at(-1), "usage: /agents stop <task_id|agent_id|all>");
    assert.deepEqual(
      backend.requests.map((entry) => entry.url),
      ["/api/v1/agents?session_id=s1"],
    );
  } finally {
    await close(backend.server);
  }
});

test("/agents steer sends a steering message to a background task", async () => {
  const backend = await startRoutesBackend((request, response) => {
    if (request.url === "/api/v1/agents/bg-3/steer" && request.method === "POST") {
      return sendJson(response, 200, { result: "accepted", message: "steering bg-3 (explorer#7)" });
    }
    if (request.url === "/api/v1/agents/bg-4/steer" && request.method === "POST") {
      return sendJson(response, 409, { error: "bg-4 already finished (done)" });
    }
    if (request.url === "/api/v1/agents/bg-9/steer" && request.method === "POST") {
      return sendJson(response, 404, { error: "unknown background task: bg-9" });
    }
    if (request.url === "/api/v1/agents/explorer%237/steer" && request.method === "POST") {
      return sendJson(response, 200, { result: "accepted", message: "steering explorer#7" });
    }
    return sendJson(response, 500, { error: `unexpected ${request.method} ${request.url}` });
  });
  try {
    const { ctx, printed } = makeContext(backend.url, "s1");
    // 带空格的纠偏文本按原样发送（token 重新拼接）。
    await runCommand(ctx, "/agents steer bg-3 只看 include/ 目录");
    assert.deepEqual(printed, ["steered bg-3"]);
    assert.deepEqual(backend.requests[0], {
      method: "POST",
      url: "/api/v1/agents/bg-3/steer",
      body: JSON.stringify({ message: "只看 include/ 目录", session_id: "s1" }),
    });

    // agent_id 形态同样接受；`#` 会转义。
    await runCommand(ctx, "/agents steer explorer#7 换个思路");
    assert.equal(printed.at(-1), "steered explorer#7");
    assert.equal(backend.requests.at(-1).url, "/api/v1/agents/explorer%237/steer");
    assert.deepEqual(JSON.parse(backend.requests.at(-1).body), {
      message: "换个思路",
      session_id: "s1",
    });

    // 409 已结束：只打印一行，不抛穿。
    await runCommand(ctx, "/agents steer bg-4 停一下");
    assert.equal(printed.at(-1), "steer failed: bg-4 already finished (done)");

    // 404 未知任务同理。
    await runCommand(ctx, "/agents steer bg-9 停一下");
    assert.equal(printed.at(-1), "steer failed: unknown background task: bg-9");
    assert.equal(backend.requests.length, 4);
  } finally {
    await close(backend.server);
  }
});

test("/agents steer requires an id and a message", async () => {
  const backend = await startRoutesBackend((request, response) =>
    sendJson(response, 500, { error: `unexpected ${request.method} ${request.url}` }),
  );
  try {
    const { ctx, printed } = makeContext(backend.url, "s1");
    await runCommand(ctx, "/agents steer");
    assert.deepEqual(printed, ["usage: /agents steer <task_id|agent_id> <message>"]);
    await runCommand(ctx, "/agents steer bg-3");
    assert.deepEqual(printed, [
      "usage: /agents steer <task_id|agent_id> <message>",
      "usage: /agents steer <task_id|agent_id> <message>",
    ]);
    assert.deepEqual(backend.requests, [], "usage errors must not hit the backend");
  } finally {
    await close(backend.server);
  }
});
