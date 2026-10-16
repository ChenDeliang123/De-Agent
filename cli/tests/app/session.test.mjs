import assert from "node:assert/strict";
import http from "node:http";
import test from "node:test";

import {
  ackAgentResults,
  approveChat,
  cancelChat,
  compactSession,
  getAgents,
  getProjectPermission,
  initProjectPermissions,
  listSessions,
  loadHistory,
  setTrust,
  steerAgent,
  steerChat,
  stopAgent,
  streamChat,
} from "../../src/app/session.mjs";

function listen(server) {
  return new Promise((resolve) => {
    server.listen(0, "127.0.0.1", () => resolve(server.address().port));
  });
}

function close(server) {
  return new Promise((resolve) => server.close(resolve));
}

test("cli session client lists, resumes, and streams", async () => {
  let chatBody = null;
  const backend = http.createServer((request, response) => {
    if (request.url === "/api/v1/sessions") {
      response.writeHead(200, { "content-type": "application/json" });
      response.end('[{"id":"s1","title":"hello world"}]');
      return;
    }
    if (request.url === "/api/v1/sessions/s1/messages") {
      response.writeHead(200, { "content-type": "application/json" });
      response.end(
        '[{"role":"user","content":"hello"},{"role":"assistant","content":"hi"}]',
      );
      return;
    }
    if (request.url === "/api/v1/chat/stream") {
      response.writeHead(200, { "content-type": "text/event-stream" });
      const chunks = [];
      request.on("data", (chunk) => chunks.push(chunk));
      request.on("end", () => {
        chatBody = JSON.parse(Buffer.concat(chunks).toString("utf8"));
        response.write('event: content\ndata: {"content":"hel"}\n\n');
        response.write(
          'event: tool_start\ndata: {"id":"call_1","name":"bash","arguments":{"command":"echo hi"}}\n\n',
        );
        response.write('event: tool_end\ndata: {"id":"call_1","ok":true,"text":"hi"}\n\n');
        response.write('event: content\ndata: {"content":"lo"}\n\n');
        response.end('event: done\ndata: {"session_id":"s1"}\n\n');
      });
      return;
    }
    response.writeHead(404);
    response.end("not found");
  });

  const port = await listen(backend);
  try {
    const sessions = await listSessions(`http://127.0.0.1:${port}`);
    assert.deepEqual(sessions, [{ id: "s1", title: "hello world" }]);

    const history = await loadHistory(`http://127.0.0.1:${port}`, "s1");
    assert.equal(history.length, 2);

    const events = [];
    const done = await streamChat(`http://127.0.0.1:${port}`, "s1", "hi", ({ event, data }) => {
      events.push({ event, data });
    });
    assert.equal(done.session_id, "s1");
    assert.equal(chatBody.message, "hi");
    assert.equal(typeof chatBody.cwd, "string");
    assert.ok(chatBody.cwd.length > 0);
    assert.deepEqual(
      events.map((entry) => entry.event),
      ["content", "tool_start", "tool_end", "content", "done"],
    );
  } finally {
    await close(backend);
  }
});

test("cli session client compacts a session", async () => {
  let compacted = false;
  let compactBody = null;
  const backend = http.createServer((request, response) => {
    if (request.url === "/api/v1/sessions/s1/compact" && request.method === "POST") {
      compacted = true;
      const chunks = [];
      request.on("data", (chunk) => chunks.push(chunk));
      request.on("end", () => {
        compactBody = JSON.parse(Buffer.concat(chunks).toString("utf8"));
        response.writeHead(200, { "content-type": "application/json" });
        response.end('{"compacted":true}');
      });
      return;
    }
    if (request.url === "/api/v1/sessions/s2/compact" && request.method === "POST") {
      response.writeHead(409, { "content-type": "application/json" });
      response.end('{"compacted":false,"error":"nothing to compact"}');
      return;
    }
    response.writeHead(404);
    response.end("not found");
  });

  const port = await listen(backend);
  try {
    const result = await compactSession(
      `http://127.0.0.1:${port}`,
      "s1",
      "/tmp/compact-project",
    );
    assert.equal(result.compacted, true);
    assert.equal(compacted, true);
    assert.equal(compactBody.cwd, "/tmp/compact-project");
    await assert.rejects(
      compactSession(`http://127.0.0.1:${port}`, "s2"),
      /nothing to compact/,
    );
  } finally {
    await close(backend);
  }
});

test("cli stream can be aborted by signal", async () => {
  let openResponse = null;
  const backend = http.createServer((request, response) => {
    if (request.url === "/api/v1/chat/stream") {
      openResponse = response;
      response.writeHead(200, { "content-type": "text/event-stream" });
      response.write('event: content\ndata: {"content":"partial"}\n\n');
      request.resume();
      return;
    }
    if (request.url === "/api/v1/chat/cancel") {
      response.writeHead(200, { "content-type": "application/json" });
      response.end('{"cancelled":true}');
      return;
    }
    response.writeHead(404);
    response.end("not found");
  });

  const port = await listen(backend);
  const controller = new AbortController();
  try {
    const stream = streamChat(
      `http://127.0.0.1:${port}`,
      null,
      "hello",
      () => {},
      process.cwd(),
      controller.signal,
    );
    setTimeout(() => controller.abort(), 50);
    await assert.rejects(stream, (error) => error.name === "AbortError");
    const cancelled = await cancelChat(`http://127.0.0.1:${port}`, "s1");
    assert.equal(cancelled.cancelled, true);
  } finally {
    if (openResponse) {
      openResponse.destroy();
    }
    await close(backend);
  }
});

test("cli session stream awaits async event handlers", async () => {
  const backend = http.createServer((request, response) => {
    if (request.url === "/api/v1/chat/stream") {
      response.writeHead(200, { "content-type": "text/event-stream" });
      request.on("data", () => {});
      request.on("end", () => {
        response.write('event: content\ndata: {"content":"x"}\n\n');
        response.end('event: done\ndata: {"session_id":"s1"}\n\n');
      });
      return;
    }
    response.writeHead(404);
    response.end("not found");
  });

  const port = await listen(backend);
  try {
    const order = [];
    await streamChat(`http://127.0.0.1:${port}`, "s1", "hi", async ({ event }) => {
      if (event === "content") {
        await new Promise((resolve) => setTimeout(resolve, 20));
        order.push("handler-done");
      }
      if (event === "done") {
        order.push("done-event");
      }
    });
    assert.deepEqual(order, ["handler-done", "done-event"]);
  } finally {
    await close(backend);
  }
});

test("cli session client sends approval decisions", async () => {
  let body = null;
  const backend = http.createServer((request, response) => {
    if (request.url === "/api/v1/chat/approve" && request.method === "POST") {
      const chunks = [];
      request.on("data", (chunk) => chunks.push(chunk));
      request.on("end", () => {
        body = JSON.parse(Buffer.concat(chunks).toString("utf8"));
        response.writeHead(200, { "content-type": "application/json" });
        response.end('{"resolved":true}');
      });
      return;
    }
    response.writeHead(404);
    response.end("not found");
  });

  const port = await listen(backend);
  try {
    const result = await approveChat(
      `http://127.0.0.1:${port}`,
      "s1",
      "call-1",
      "allow_once",
    );
    assert.deepEqual(result, { resolved: true });
    assert.deepEqual(body, {
      session_id: "s1",
      tool_call_id: "call-1",
      decision: "allow_once",
    });
  } finally {
    await close(backend);
  }
});

test("cli session client reads project permission, sets trust, and inits", async () => {
  const requests = [];
  const backend = http.createServer((request, response) => {
    const chunks = [];
    request.on("data", (chunk) => chunks.push(chunk));
    request.on("end", () => {
      const body = Buffer.concat(chunks).toString("utf8");
      requests.push({ method: request.method, url: request.url, body });
      const send = (payload) => {
        response.writeHead(200, { "content-type": "application/json" });
        response.end(JSON.stringify(payload));
      };
      if (request.url.startsWith("/api/v1/permissions/project")) {
        return send({ path: "/tmp/proj", root: "/tmp/proj", has_config: true, trusted: false });
      }
      if (request.url === "/api/v1/permissions/trust" && request.method === "PUT") {
        return send({ root: "/tmp/proj", trusted: true });
      }
      if (request.url === "/api/v1/permissions/init" && request.method === "POST") {
        return send({ root: "/tmp/proj", created: ["permissions.json"] });
      }
      response.writeHead(404);
      response.end("not found");
    });
  });

  const port = await listen(backend);
  try {
    const base = `http://127.0.0.1:${port}`;
    const info = await getProjectPermission(base, "/tmp/proj/sub");
    assert.equal(info.root, "/tmp/proj");
    assert.equal(info.has_config, true);
    assert.equal(info.trusted, false);

    const trust = await setTrust(base, "/tmp/proj", true);
    assert.deepEqual(trust, { root: "/tmp/proj", trusted: true });
    assert.deepEqual(JSON.parse(requests.at(-1).body), { path: "/tmp/proj", trusted: true });

    const init = await initProjectPermissions(base, "/tmp/proj", true);
    assert.deepEqual(init, { root: "/tmp/proj", created: ["permissions.json"] });
    assert.deepEqual(JSON.parse(requests.at(-1).body), {
      path: "/tmp/proj",
      seed_from_global: true,
    });
  } finally {
    await close(backend);
  }
});

test("cli session client lists, stops, and acks background agents", async () => {
  const requests = [];
  const PENDING = {
    task_id: "bg-4",
    agent_id: "explorer#8",
    agent_type: "explorer",
    session_id: "s1",
    state: "done",
    ok: true,
    duration_ms: 1000,
    tool_calls: 1,
    prompt_tokens: 10,
    completion_tokens: 5,
    cache_hit_tokens: 0,
    text: "report",
    error: null,
  };
  const backend = http.createServer((request, response) => {
    const chunks = [];
    request.on("data", (chunk) => chunks.push(chunk));
    request.on("end", () => {
      const body = Buffer.concat(chunks).toString("utf8");
      requests.push({ method: request.method, url: request.url, body });
      const send = (status, payload) => {
        response.writeHead(status, { "content-type": "application/json" });
        response.end(JSON.stringify(payload));
      };
      if (request.url === "/api/v1/agents" && request.method === "GET") {
        return send(200, { enabled: true, max_background: 2, running: [], pending_results: [] });
      }
      if (request.url === "/api/v1/agents?session_id=s1" && request.method === "GET") {
        return send(200, {
          enabled: true,
          max_background: 2,
          running: [
            {
              task_id: "bg-3",
              agent_id: "explorer#7",
              agent_type: "explorer",
              session_id: "s1",
              state: "running",
              elapsed_ms: 12300,
              prompt_excerpt: "scan cli",
            },
          ],
          pending_results: [PENDING],
        });
      }
      if (request.url === "/api/v1/agents?session_id=boom" && request.method === "GET") {
        return send(500, {});
      }
      if (request.url === "/api/v1/agents/bg-3/stop" && request.method === "POST") {
        return send(200, { result: "requested", message: "stopping bg-3 (explorer#7)" });
      }
      if (request.url === "/api/v1/agents/bg-9/stop" && request.method === "POST") {
        return send(404, { error: "unknown background task: bg-9" });
      }
      if (request.url === "/api/v1/agents/bg-500/stop" && request.method === "POST") {
        return send(500, {});
      }
      if (request.url === "/api/v1/agents/ack" && request.method === "POST") {
        const payload = JSON.parse(body || "{}");
        if (payload.task_ids?.[0] === "bg-500") {
          return send(500, {});
        }
        return send(200, { acked: payload.task_ids?.length ?? 0 });
      }
      return send(500, { error: `unexpected ${request.method} ${request.url}` });
    });
  });

  const port = await listen(backend);
  try {
    const base = `http://127.0.0.1:${port}`;
    // 无 session_id：只打 /api/v1/agents，后端返回全部会话。
    const all = await getAgents(base);
    assert.equal(requests[0].method, "GET");
    assert.equal(requests[0].url, "/api/v1/agents");
    assert.equal(all.enabled, true);
    assert.equal(all.max_background, 2);

    // 带 session_id：query 只含该会话的运行中/待补打任务。
    const scoped = await getAgents(base, "s1");
    assert.equal(requests[1].url, "/api/v1/agents?session_id=s1");
    assert.equal(scoped.running[0].task_id, "bg-3");
    assert.deepEqual(scoped.pending_results, [PENDING]);

    await assert.rejects(getAgents(base, "boom"), /failed to load agents: HTTP 500/);

    const stop = await stopAgent(base, "bg-3");
    assert.deepEqual(stop, { result: "requested", message: "stopping bg-3 (explorer#7)" });
    assert.equal(requests[3].method, "POST");
    assert.equal(requests[3].url, "/api/v1/agents/bg-3/stop");
    assert.deepEqual(JSON.parse(requests[3].body), {});

    // 404 的 body.error 原样透出；500 无 error 时退回状态码文案。
    await assert.rejects(stopAgent(base, "bg-9"), /unknown background task: bg-9/);
    await assert.rejects(stopAgent(base, "bg-500"), /stop agent failed: HTTP 500/);

    // agent_id 形态（含 `#`）必须转义，否则 URL 会被截断。
    await assert.rejects(stopAgent(base, "explorer#7"), /unexpected POST/);
    assert.equal(requests[6].url, "/api/v1/agents/explorer%237/stop");

    const ack = await ackAgentResults(base, ["bg-3", "bg-4"]);
    assert.deepEqual(ack, { acked: 2 });
    assert.equal(requests[7].method, "POST");
    assert.equal(requests[7].url, "/api/v1/agents/ack");
    assert.deepEqual(JSON.parse(requests[7].body), { task_ids: ["bg-3", "bg-4"] });
    await assert.rejects(ackAgentResults(base, ["bg-500"]), /ack agents failed: HTTP 500/);
  } finally {
    await close(backend);
  }
});

test("cli session client steers a background agent", async () => {
  const requests = [];
  const backend = http.createServer((request, response) => {
    const chunks = [];
    request.on("data", (chunk) => chunks.push(chunk));
    request.on("end", () => {
      const body = Buffer.concat(chunks).toString("utf8");
      requests.push({ method: request.method, url: request.url, body });
      const send = (status, payload) => {
        response.writeHead(status, { "content-type": "application/json" });
        response.end(JSON.stringify(payload));
      };
      const steerAccepted = () => send(200, { result: "accepted", message: "steering bg-3 (explorer#7)" });
      if (request.url === "/api/v1/agents/bg-3/steer" && request.method === "POST") {
        return steerAccepted();
      }
      if (request.url === "/api/v1/agents/bg-4/steer" && request.method === "POST") {
        return steerAccepted();
      }
      if (request.url === "/api/v1/agents/explorer%237/steer" && request.method === "POST") {
        return steerAccepted();
      }
      if (request.url === "/api/v1/agents/bg-400/steer" && request.method === "POST") {
        return send(400, { error: "steer message is required" });
      }
      if (request.url === "/api/v1/agents/bg-404/steer" && request.method === "POST") {
        return send(404, { error: "unknown background task: bg-404" });
      }
      if (request.url === "/api/v1/agents/bg-409/steer" && request.method === "POST") {
        return send(409, { error: "background task inbox is full (8)" });
      }
      if (request.url === "/api/v1/agents/bg-500/steer" && request.method === "POST") {
        return send(500, {});
      }
      return send(500, { error: `unexpected ${request.method} ${request.url}` });
    });
  });

  const port = await listen(backend);
  try {
    const base = `http://127.0.0.1:${port}`;
    // 带 session_id：body 里包含 session_id。
    const accepted = await steerAgent(base, "bg-3", "只看 include/ 目录", "s1");
    assert.deepEqual(accepted, { result: "accepted", message: "steering bg-3 (explorer#7)" });
    assert.equal(requests[0].method, "POST");
    assert.equal(requests[0].url, "/api/v1/agents/bg-3/steer");
    assert.deepEqual(JSON.parse(requests[0].body), {
      message: "只看 include/ 目录",
      session_id: "s1",
    });

    // 不带 session_id：body 里不出现该字段。
    await steerAgent(base, "bg-4", "换个思路");
    assert.deepEqual(JSON.parse(requests[1].body), { message: "换个思路" });

    // agent_id 形态（含 `#`）转义。
    await steerAgent(base, "explorer#7", "hi", "s1");
    assert.equal(requests[2].url, "/api/v1/agents/explorer%237/steer");

    // 400/404/409 的 body.error 原样透出；500 无 error 时退回状态码文案。
    await assert.rejects(steerAgent(base, "bg-400", "", "s1"), /steer message is required/);
    await assert.rejects(steerAgent(base, "bg-404", "hi"), /unknown background task: bg-404/);
    await assert.rejects(steerAgent(base, "bg-409", "hi", "s1"), /background task inbox is full \(8\)/);
    await assert.rejects(steerAgent(base, "bg-500", "hi"), /steer failed: HTTP 500/);
    assert.equal(requests.length, 7);
    assert.equal(requests[6].url, "/api/v1/agents/bg-500/steer");
  } finally {
    await close(backend);
  }
});

test("cli session client steers the running turn", async () => {
  const requests = [];
  const backend = http.createServer((request, response) => {
    const chunks = [];
    request.on("data", (chunk) => chunks.push(chunk));
    request.on("end", () => {
      const body = Buffer.concat(chunks).toString("utf8");
      requests.push({ method: request.method, url: request.url, body });
      const send = (status, payload) => {
        response.writeHead(status, { "content-type": "application/json" });
        response.end(JSON.stringify(payload));
      };
      if (request.url !== "/api/v1/chat/steer" || request.method !== "POST") {
        return send(500, { error: `unexpected ${request.method} ${request.url}` });
      }
      const message = JSON.parse(body || "{}").message ?? "";
      if (message === "ok") {
        return send(200, { accepted: true });
      }
      if (message === "required") {
        return send(400, { error: "steer message is required" });
      }
      if (message === "long") {
        return send(400, { error: "steer message is too long (4096 bytes max)" });
      }
      if (message === "norun") {
        return send(409, { error: "no running turn for this session" });
      }
      if (message === "full") {
        return send(409, { error: "steer inbox is full (8)" });
      }
      return send(500, {});
    });
  });

  const port = await listen(backend);
  try {
    const base = `http://127.0.0.1:${port}`;
    const accepted = await steerChat(base, "s1", "ok");
    assert.deepEqual(accepted, { accepted: true });
    assert.equal(requests[0].method, "POST");
    assert.equal(requests[0].url, "/api/v1/chat/steer");
    assert.deepEqual(JSON.parse(requests[0].body), { session_id: "s1", message: "ok" });

    // 中文正文按原样编码进 body（该消息不在假后端白名单里，故意让它 500 以便只断言 body）。
    await steerChat(base, "s1", "只看 include/ 目录").catch(() => {});
    assert.deepEqual(JSON.parse(requests[1].body), {
      session_id: "s1",
      message: "只看 include/ 目录",
    });

    // 400/409 的 body.error 原样透出；500 无 error 时退回状态码文案。
    await assert.rejects(steerChat(base, "s1", "required"), /steer message is required/);
    await assert.rejects(
      steerChat(base, "s1", "long"),
      /steer message is too long \(4096 bytes max\)/,
    );
    await assert.rejects(steerChat(base, "s1", "norun"), /no running turn for this session/);
    await assert.rejects(steerChat(base, "s1", "full"), /steer inbox is full \(8\)/);
    await assert.rejects(steerChat(base, "s1", "boom"), /steer failed: HTTP 500/);
    assert.equal(requests.length, 7);
  } finally {
    await close(backend);
  }
});
