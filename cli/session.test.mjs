import assert from "node:assert/strict";
import http from "node:http";
import test from "node:test";

import { cancelChat, listSessions, loadHistory, streamChat } from "./session.mjs";

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
