import assert from "node:assert/strict";
import http from "node:http";
import test from "node:test";

import { createHttpServer } from "./server.js";

function listen(server) {
  return new Promise((resolve) => {
    server.listen(0, "127.0.0.1", () => resolve(server.address().port));
  });
}

function close(server) {
  return new Promise((resolve) => server.close(resolve));
}

test("node frontend proxies sessions and static page", async () => {
  const backend = http.createServer((request, response) => {
    if (request.url === "/api/v1/sessions") {
      response.writeHead(200, { "content-type": "application/json" });
      response.end('["session-1"]');
      return;
    }
    if (request.url === "/api/v1/chat/stream") {
      response.writeHead(200, { "content-type": "text/event-stream" });
      request.resume();
      request.on("end", () => {
        response.write('event: content\ndata: {"content":"你好"}\n\n');
        response.end('event: done\ndata: {"session_id":"session-1"}\n\n');
      });
      return;
    }
    response.writeHead(404);
    response.end("not found");
  });

  const backendPort = await listen(backend);
  const app = createHttpServer(`http://127.0.0.1:${backendPort}`);
  const appPort = await listen(app);

  try {
    const page = await fetch(`http://127.0.0.1:${appPort}/`);
    assert.equal(page.status, 200);
    assert.match(await page.text(), /AI Agent/);

    const sessions = await fetch(`http://127.0.0.1:${appPort}/api/sessions`);
    assert.equal(sessions.status, 200);
    assert.deepEqual(await sessions.json(), ["session-1"]);

    const stream = await fetch(`http://127.0.0.1:${appPort}/api/chat/stream`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ message: "hello" }),
    });
    const streamText = await stream.text();
    assert.match(streamText, /event: content/);
    assert.match(streamText, /你好/);
  } finally {
    await close(app);
    await close(backend);
  }
});
