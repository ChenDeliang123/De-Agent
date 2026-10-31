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

function makeContext(backend, cwd = "/tmp/project") {
  const printed = [];
  const ctx = {
    backend,
    state: { sessionId: null, cwd, shouldExit: false },
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

const ENABLED = {
  enabled: true,
  truncated: false,
  skills: [
    {
      name: "release-notes",
      description: "Write release notes from git history",
      source: "project",
      path: "/repo/.de/skills/release-notes/SKILL.md",
    },
    {
      name: "pdf-processing",
      description: "Extract text from PDF files",
      source: "user",
      path: "/home/user/.de/skills/pdf-processing/SKILL.md",
    },
  ],
};

test("/skills prints the catalog and passes cwd", async () => {
  const backend = await startBackend(200, ENABLED);
  try {
    const { ctx, printed } = makeContext(backend.url);

    assert.equal(await runCommand(ctx, "/skills"), true);
    assert.deepEqual(backend.requests, [
      { method: "GET", url: "/api/v1/skills?path=%2Ftmp%2Fproject" },
    ]);
    const text = printed.join("\n");
    assert.ok(printed.includes("skills (2):"), text);
    assert.ok(
      printed.some(
        (line) =>
          line.includes("release-notes") &&
          line.includes("project") &&
          line.includes("Write release notes from git history"),
      ),
      text,
    );
    assert.ok(
      printed.some(
        (line) =>
          line.includes("pdf-processing") &&
          line.includes("user") &&
          line.includes("Extract text from PDF files"),
      ),
      text,
    );
  } finally {
    await close(backend.server);
  }
});

test("/skills reports disabled and empty catalogs", async () => {
  {
    const backend = await startBackend(200, { enabled: false, skills: [] });
    try {
      const { ctx, printed } = makeContext(backend.url);
      await runCommand(ctx, "/skills");
      assert.deepEqual(printed, ["skills disabled ([skills] enabled=false)"]);
    } finally {
      await close(backend.server);
    }
  }

  {
    const backend = await startBackend(200, { enabled: true, skills: [] });
    try {
      const { ctx, printed } = makeContext(backend.url);
      await runCommand(ctx, "/skills");
      assert.deepEqual(printed, ["(no skills)"]);
    } finally {
      await close(backend.server);
    }
  }
});

test("/skills marks a truncated catalog", async () => {
  const backend = await startBackend(200, { ...ENABLED, truncated: true });
  try {
    const { ctx, printed } = makeContext(backend.url);
    await runCommand(ctx, "/skills");
    assert.ok(printed.includes("  (catalog truncated)"), printed.join("\n"));
  } finally {
    await close(backend.server);
  }
});

test("/skills reports backend failures", async () => {
  const backend = await startBackend(500, { error: "boom" });
  try {
    const { ctx, printed } = makeContext(backend.url);
    await runCommand(ctx, "/skills");
    assert.deepEqual(printed, ["skills failed: failed to list skills: HTTP 500"]);
  } finally {
    await close(backend.server);
  }
});

test("/skills is registered under info and shows up in /help", () => {
  const command = commands.find((entry) => entry.name === "skills");
  assert.ok(command, "missing /skills command");
  assert.equal(command.category, "info");
  assert.equal(command.argHint, "");
  assert.ok(renderHelp().includes("/skills"), "/help should list /skills");
});
