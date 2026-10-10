import assert from "node:assert/strict";
import http from "node:http";
import os from "node:os";
import test from "node:test";

import { commands, runCommand } from "../../src/app/commands/index.mjs";

function listen(server) {
  return new Promise((resolve) => {
    server.listen(0, "127.0.0.1", () => resolve(server.address().port));
  });
}

function close(server) {
  return new Promise((resolve) => server.close(resolve));
}

function readBody(request) {
  return new Promise((resolve) => {
    const chunks = [];
    request.on("data", (chunk) => chunks.push(chunk));
    request.on("end", () => resolve(Buffer.concat(chunks).toString("utf8")));
  });
}

function startBackend() {
  const requests = [];
  const server = http.createServer(async (request, response) => {
    const body = await readBody(request);
    requests.push({ method: request.method, url: request.url, body });
    const send = (status, payload) => {
      response.writeHead(status, { "content-type": "application/json" });
      response.end(JSON.stringify(payload));
    };
    if (request.url === "/health") {
      return send(200, { status: "ok" });
    }
    if (request.url === "/api/v1/tools") {
      return send(200, [
        { name: "read", description: "Read a file" },
        { name: "bash", description: "Run a command" },
      ]);
    }
    if (request.url === "/api/v1/sessions" && request.method === "GET") {
      return send(200, [
        { id: "s1", title: "hello", updated_at: 2 },
        { id: "s2", title: "world", updated_at: 1 },
      ]);
    }
    if (request.url === "/api/v1/sessions/s1/stats") {
      return send(200, {
        id: "s1",
        messages: 4,
        user_messages: 2,
        assistant_messages: 2,
        characters: 40,
        tasks: 3,
        compaction: { tokens_before: 100, tokens_after: 20 },
        cwd: "/tmp/restored-project",
        instructions: [
          { path: "/tmp/restored-project/AGENTS.md", bytes: 32, truncated: false },
          { path: "/tmp/restored-project/sub/AGENTS.md", bytes: 64, truncated: true },
        ],
      });
    }
    if (request.url === "/api/v1/sessions/s1/messages") {
      return send(200, []);
    }
    if (request.url === "/api/v1/sessions/s3/messages") {
      return send(200, []);
    }
    if (request.url === "/api/v1/sessions/s1" && request.method === "PATCH") {
      const parsed = JSON.parse(body || "{}");
      return send(200, { id: "s1", title: parsed.title });
    }
    if (request.url === "/api/v1/sessions/s1" && request.method === "DELETE") {
      return send(200, { deleted: true, id: "s1" });
    }
    if (request.url === "/api/v1/sessions/s1/clone" && request.method === "POST") {
      return send(200, { session_id: "s3" });
    }
    return send(404, { error: "not found" });
  });
  return { server, requests };
}

function makeContext(backend, overrides = {}) {
  const printed = [];
  const rendered = [];
  const { state: stateOverride, ...rest } = overrides;
  const state = {
    sessionId: null,
    cwd: process.cwd(),
    cwdExplicit: false,
    shouldExit: false,
    ...(stateOverride ?? {}),
  };
  const ctx = {
    backend,
    state,
    commands,
    print: (line) => printed.push(line),
    log: () => {},
    clearInputLine: () => {},
    clearScreen: () => {},
    renderHistory: async (_backend, sessionId) => rendered.push(sessionId),
    ...rest,
  };
  return { ctx, printed, rendered, state };
}

async function withBackend(run) {
  const { server, requests } = startBackend();
  const port = await listen(server);
  try {
    await run(`http://127.0.0.1:${port}`, requests);
  } finally {
    await close(server);
  }
}

test("resume with the selector opens the chosen session", async () => {
  await withBackend(async (backend) => {
    const { ctx, state, rendered } = makeContext(backend, {
      selectSession: async (sessions) => {
        assert.equal(sessions.length, 2);
        return 1;
      },
    });
    await runCommand(ctx, "/resume");
    assert.equal(state.sessionId, "s2");
    assert.deepEqual(rendered, ["s2"]);
  });
});

test("resume by index and by id both open a session", async () => {
  await withBackend(async (backend) => {
    const first = makeContext(backend);
    await runCommand(first.ctx, "/resume 1");
    assert.equal(first.state.sessionId, "s1");
    assert.equal(first.state.cwd, "/tmp/restored-project");

    const second = makeContext(backend);
    await runCommand(second.ctx, "/open s2");
    assert.equal(second.state.sessionId, "s2");
  });
});

test("name renames the current session", async () => {
  await withBackend(async (backend) => {
    const { ctx, printed, state } = makeContext(backend, {
      state: { sessionId: "s1" },
    });
    await runCommand(ctx, "/name my task");
    assert.match(printed.at(-1), /renamed: my task/);
    assert.equal(state.sessionId, "s1");
  });
});

test("clone switches to the cloned session", async () => {
  await withBackend(async (backend) => {
    const { ctx, printed, state, rendered } = makeContext(backend, {
      state: { sessionId: "s1" },
    });
    await runCommand(ctx, "/clone");
    assert.equal(state.sessionId, "s3");
    assert.match(printed.at(-1), /cloned to session s3/);
    assert.deepEqual(rendered, ["s3"]);
  });
});

test("delete asks for confirmation and honors --yes", async () => {
  await withBackend(async (backend) => {
    const refused = makeContext(backend, {
      state: { sessionId: "s1" },
      confirm: async () => false,
    });
    await runCommand(refused.ctx, "/delete");
    assert.match(refused.printed.at(-1), /cancelled/);
    assert.equal(refused.state.sessionId, "s1");

    const forced = makeContext(backend, { state: { sessionId: "s1" } });
    await runCommand(forced.ctx, "/delete --yes");
    assert.match(forced.printed.at(-1), /deleted session s1/);
    assert.equal(forced.state.sessionId, null);
  });
});

test("session prints stats and compaction info", async () => {
  await withBackend(async (backend) => {
    const { ctx, printed } = makeContext(backend, {
      state: { sessionId: "s1" },
    });
    await runCommand(ctx, "/session");
    assert.ok(printed.some((line) => line.includes("messages:   4")));
    assert.ok(printed.some((line) => line.includes("compaction: 100")));
    assert.ok(printed.some((line) => line.includes("cwd:        /tmp/restored-project")));
    assert.ok(printed.some((line) => line.includes("instructions: 2 file(s)")));
    assert.ok(printed.some((line) => line.includes("/tmp/restored-project/sub/AGENTS.md (truncated)")));
  });
});

test("status reports backend health", async () => {
  await withBackend(async (backend) => {
    const { ctx, printed } = makeContext(backend);
    await runCommand(ctx, "/status");
    assert.ok(printed.some((line) => line.includes("backend:  ok")));
    assert.ok(printed.some((line) => line.includes("sessions: 2")));
  });
});

test("tools lists the agent tools", async () => {
  await withBackend(async (backend) => {
    const { ctx, printed } = makeContext(backend);
    await runCommand(ctx, "/tools");
    assert.ok(printed.some((line) => line.includes("read")));
    assert.ok(printed.some((line) => line.includes("bash")));
  });
});

test("cwd shows and changes the working directory", async () => {
  const { ctx, printed, state } = makeContext("http://127.0.0.1:1");
  await runCommand(ctx, "/cwd");
  assert.match(printed[0], /^cwd: /);

  await runCommand(ctx, "/cwd /definitely/not/a/real/dir");
  assert.match(printed.at(-1), /directory not found/);

  const target = os.tmpdir();
  await runCommand(ctx, `/cwd ${target}`);
  assert.equal(state.cwd, target);
  assert.equal(state.cwdExplicit, true);
  assert.match(printed.at(-1), /^cwd: /);
});
