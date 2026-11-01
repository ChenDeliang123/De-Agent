import assert from "node:assert/strict";
import http from "node:http";
import test from "node:test";

import {
  commands,
  findCommand,
  renderHelp,
  runCommand,
} from "../../src/app/commands/index.mjs";
import { parseReviewPrepareArgs } from "../../src/app/review-prepare.mjs";
import { reviewPrepare } from "../../src/app/review-prepare.mjs";

function listen(server) {
  return new Promise((resolve) => server.listen(0, "127.0.0.1", () => resolve(server.address().port)));
}

function close(server) {
  return new Promise((resolve) => server.close(resolve));
}

function makeContext(overrides = {}) {
  const printed = [];
  const state = { sessionId: "s1", cwd: "/repo", shouldExit: false, ...(overrides.state ?? {}) };
  return {
    ctx: {
      backend: "http://127.0.0.1:1",
      state,
      commands,
      print: (line) => printed.push(line),
      log: () => {},
      ...overrides,
    },
    printed,
    state,
  };
}

test("/review is registered and documented", () => {
  const command = findCommand("review");
  assert.equal(command.name, "review");
  assert.ok(commands.some((entry) => entry.name === "review"));
  const help = renderHelp();
  assert.ok(help.includes("/review"));
  assert.ok(help.includes("Review:"));
});

test("/review without a subcommand prints the two-stage workflow", async () => {
  const { ctx, printed } = makeContext();
  assert.equal(await runCommand(ctx, "/review"), true);
  const text = printed.join("\n");
  assert.ok(text.includes("de review prepare"));
  assert.ok(text.includes("review_verify") || text.includes("review_setup"));
});

test("/review prepare requires a feature", async () => {
  const { ctx, printed } = makeContext();
  await runCommand(ctx, "/review prepare");
  assert.ok(printed.join("\n").includes("usage: /review prepare"));
});

test("/review prepare posts producer/session/cwd", async () => {
  let createBody = null;
  const backend = http.createServer((request, response) => {
    if (request.url === "/api/v1/reviews/config") {
      response.writeHead(200, { "content-type": "application/json" });
      response.end('{"enabled":true}');
      return;
    }
    if (request.url === "/api/v1/reviews" && request.method === "POST") {
      const chunks = [];
      request.on("data", (chunk) => chunks.push(chunk));
      request.on("end", () => {
        createBody = JSON.parse(Buffer.concat(chunks).toString("utf8"));
        response.writeHead(200, { "content-type": "application/json" });
        response.end(
          JSON.stringify({
            review_id: "rv-1",
            pack_path: "docs/review/inbox/feature-x.json",
            report_path_hint: "docs/review/feature-x.md",
            changed_files: 1,
            diff_bytes: 12,
            intent_source: "de-session",
          }),
        );
      });
      return;
    }
    response.writeHead(404);
    response.end("not found");
  });
  const port = await listen(backend);
  try {
    const { ctx, printed } = makeContext({ state: { sessionId: "s1", cwd: "/repo" } });
    ctx.backend = `http://127.0.0.1:${port}`;
    await runCommand(ctx, "/review prepare feature-x --base master --context docs/plan.md");
    assert.ok(printed.join("\n").includes("de review docs/review/inbox/feature-x.json"));
    assert.equal(createBody.feature, "feature-x");
    assert.equal(createBody.base, "master");
    assert.equal(createBody.producer, "de-session");
    assert.equal(createBody.session_id, "s1");
    assert.equal(createBody.cwd, "/repo");
    assert.deepEqual(createBody.context_paths, ["docs/plan.md"]);
  } finally {
    await close(backend);
  }
});

test("/review cancel calls the cancel endpoint", async () => {
  let cancelledSession = null;
  const backend = http.createServer((request, response) => {
    if (request.url === "/api/v1/chat/cancel" && request.method === "POST") {
      const chunks = [];
      request.on("data", (chunk) => chunks.push(chunk));
      request.on("end", () => {
        cancelledSession = JSON.parse(Buffer.concat(chunks).toString("utf8")).session_id;
        response.writeHead(200, { "content-type": "application/json" });
        response.end('{"cancelled":true}');
      });
      return;
    }
    response.writeHead(404);
    response.end("not found");
  });
  const port = await listen(backend);
  try {
    const { ctx } = makeContext({ state: { sessionId: "s1", cwd: "/repo" } });
    ctx.backend = `http://127.0.0.1:${port}`;
    await runCommand(ctx, "/review cancel");
    assert.equal(cancelledSession, "s1");
  } finally {
    await close(backend);
  }
});

test("review prepare parser supports external-agent flags", () => {
  const options = parseReviewPrepareArgs([
    "--feature",
    "skill-loader",
    "--base",
    "master",
    "--request-file",
    ".de/review/request.md",
    "--context",
    "docs/plan.md",
    "--context",
    "docs/other.md",
    "--agent",
    "claude-code",
    "--json",
  ]);
  assert.equal(options.feature, "skill-loader");
  assert.equal(options.base, "master");
  assert.equal(options.requestFile, ".de/review/request.md");
  assert.deepEqual(options.contextPaths, ["docs/plan.md", "docs/other.md"]);
  assert.equal(options.agent, "claude-code");
  assert.equal(options.json, true);
});

test("reviewPrepare rejects request and request-file together", async () => {
  await assert.rejects(
    reviewPrepare({
      backend: "http://127.0.0.1:1",
      feature: "x",
      request: "inline",
      requestFile: ".de/review/request.md",
    }),
    /mutually exclusive/,
  );
});
