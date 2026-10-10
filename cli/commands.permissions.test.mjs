import assert from "node:assert/strict";
import http from "node:http";
import test from "node:test";

import { commands, runCommand } from "./commands.mjs";

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
  let currentMode = "confirm";
  const server = http.createServer(async (request, response) => {
    const body = await readBody(request);
    requests.push({ method: request.method, url: request.url, body });
    const send = (status, payload) => {
      response.writeHead(status, { "content-type": "application/json" });
      response.end(JSON.stringify(payload));
    };
    if (request.url.startsWith("/api/v1/permissions/effective")) {
      const path = decodeURIComponent(request.url.split("path=")[1] ?? "");
      return send(200, { path, mode: currentMode, source: "exact" });
    }
    if (request.url === "/api/v1/permissions" && request.method === "PUT") {
      const parsed = JSON.parse(body || "{}");
      currentMode = parsed.mode;
      return send(200, { path: parsed.path, mode: parsed.mode, source: "exact" });
    }
    if (request.url.startsWith("/api/v1/permissions") && request.method === "DELETE") {
      currentMode = "confirm";
      return send(200, { removed: true });
    }
    if (request.url === "/api/v1/permissions" && request.method === "GET") {
      return send(200, {
        default_mode: "confirm",
        paths: [{ path: "/tmp/perm-test", mode: currentMode }],
      });
    }
    return send(404, { error: "not found" });
  });
  return { server, requests };
}

function makeContext(backend, overrides = {}) {
  const printed = [];
  const state = {
    sessionId: null,
    cwd: "/tmp/perm-test",
    cwdExplicit: true,
    shouldExit: false,
    ...(overrides.state ?? {}),
  };
  const { state: _ignored, ...rest } = overrides;
  const ctx = {
    backend,
    state,
    commands,
    print: (line) => printed.push(line),
    log: () => {},
    clearInputLine: () => {},
    clearScreen: () => {},
    renderHistory: async () => {},
    ...rest,
  };
  return { ctx, printed };
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

test("permissions <mode> sets the mode for the current cwd", async () => {
  await withBackend(async (backend, requests) => {
    const { ctx, printed } = makeContext(backend);
    await runCommand(ctx, "/permissions workspace");
    const put = requests.find((r) => r.method === "PUT");
    assert.ok(put, "PUT request should be sent");
    assert.deepEqual(JSON.parse(put.body), { path: "/tmp/perm-test", mode: "workspace" });
    assert.ok(printed.some((line) => line.includes("workspace")), "should print the mode");
  });
});

test("permissions full requires confirmation and can be cancelled", async () => {
  await withBackend(async (backend, requests) => {
    const { ctx, printed } = makeContext(backend, {
      confirm: async () => false,
    });
    await runCommand(ctx, "/permissions full");
    assert.equal(requests.filter((r) => r.method === "PUT").length, 0);
    assert.ok(printed.some((line) => line.includes("cancelled")));
  });
});

test("permissions full applies after confirmation", async () => {
  await withBackend(async (backend, requests) => {
    const { ctx, printed } = makeContext(backend, {
      confirm: async () => true,
    });
    await runCommand(ctx, "/permissions danger");
    const put = requests.find((r) => r.method === "PUT");
    assert.ok(put, "PUT request should be sent after confirmation");
    assert.equal(JSON.parse(put.body).mode, "full");
    assert.ok(printed.some((line) => line.includes("FULL ACCESS")));
  });
});

test("permissions picker applies the selected mode", async () => {
  await withBackend(async (backend, requests) => {
    const { ctx, printed } = makeContext(backend, {
      // 当前为 confirm，列表顺序 [confirm, workspace, full] -> 选 index 1。
      select: async (items) => {
        assert.equal(items.length, 3);
        assert.ok(items[0].description.includes("(current)"));
        return 1;
      },
    });
    await runCommand(ctx, "/permissions");
    const put = requests.find((r) => r.method === "PUT");
    assert.ok(put, "picker should set the selected mode");
    assert.equal(JSON.parse(put.body).mode, "workspace");
    assert.ok(printed.some((line) => line.includes("workspace")));
  });
});

test("permissions show prints the effective mode", async () => {
  await withBackend(async (backend) => {
    const { ctx, printed } = makeContext(backend);
    await runCommand(ctx, "/permissions show");
    assert.ok(printed.some((line) => line.includes("mode:")));
    assert.ok(printed.some((line) => line.includes("confirm")));
  });
});

test("permissions list prints default and configured paths", async () => {
  await withBackend(async (backend) => {
    const { ctx, printed } = makeContext(backend);
    await runCommand(ctx, "/permissions list");
    assert.ok(printed.some((line) => line.includes("default: confirm")));
    assert.ok(printed.some((line) => line.includes("/tmp/perm-test")));
  });
});

test("permissions reset issues DELETE and reports the new effective mode", async () => {
  await withBackend(async (backend, requests) => {
    const { ctx, printed } = makeContext(backend);
    await runCommand(ctx, "/permissions reset");
    assert.ok(requests.some((r) => r.method === "DELETE"));
    assert.ok(printed.some((line) => line.includes("cleared")));
  });
});

test("permissions rejects unknown mode names", async () => {
  await withBackend(async (backend, requests) => {
    const { ctx, printed } = makeContext(backend);
    await runCommand(ctx, "/permissions bogus");
    assert.equal(requests.filter((r) => r.method === "PUT").length, 0);
    assert.ok(printed.some((line) => line.includes("unknown mode")));
  });
});
