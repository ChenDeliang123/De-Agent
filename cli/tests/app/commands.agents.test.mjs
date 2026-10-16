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
  stats: { runs: 3, failures: 1, cancelled: 0, total_ms: 12345 },
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

function makeContext(backend) {
  const printed = [];
  const ctx = {
    backend,
    state: { sessionId: null, cwd: "/tmp", shouldExit: false },
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

test("/agents prints settings, definitions, and stats", async () => {
  const backend = await startBackend(200, ENABLED);
  try {
    const { ctx, printed } = makeContext(backend.url);
    assert.equal(await runCommand(ctx, "/agents"), true);
    assert.deepEqual(backend.requests, [{ method: "GET", url: "/api/v1/agents" }]);
    const text = printed.join("\n");
    assert.ok(
      printed.includes("sub-agents enabled: max_depth=1 max_concurrent=3"),
      `missing limits line: ${text}`,
    );
    assert.ok(
      printed.includes("stats: runs=3 failures=1 cancelled=0 total_ms=12345"),
      `missing stats line: ${text}`,
    );
    assert.ok(printed.includes("definitions:"), `missing definitions header: ${text}`);
    const general = printed.find((line) => line.includes("general"));
    assert.ok(
      general.includes("model: (default)") && general.includes("tools: (all)"),
      `general definition should fall back to defaults: ${general}`,
    );
    const explorer = printed.find((line) => line.includes("explorer"));
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
  assert.ok(renderHelp().includes("/agents"), "/help should list /agents");
});
