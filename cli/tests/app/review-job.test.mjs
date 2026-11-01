import assert from "node:assert/strict";
import http from "node:http";
import path from "node:path";
import test from "node:test";

import {
  parseReviewJobArgs,
  reviewJobHelp,
  runReviewJob,
} from "../../src/app/review-job.mjs";

function listen(server) {
  return new Promise((resolve) =>
    server.listen(0, "127.0.0.1", () => resolve(server.address().port)),
  );
}

function close(server) {
  return new Promise((resolve) => server.close(resolve));
}

function job(overrides = {}) {
  return {
    job_id: "rvjob-1",
    review_id: "rv-1",
    feature: "feature-x",
    branch: "main",
    state: "queued",
    report_path: "",
    verdict: "",
    progress: { files_done: 0, files_total: 2, findings: 0 },
    ...overrides,
  };
}

test("review job parser supports submit/status/cancel/attach", () => {
  assert.deepEqual(parseReviewJobArgs(["submit", "/repo/p.json", "--json"]), {
    backend: "http://127.0.0.1:18081",
    sessionId: null,
    json: true,
    command: "submit",
    target: "/repo/p.json",
  });
  const status = parseReviewJobArgs(["status", "rvjob-1", "--session", "s1"]);
  assert.equal(status.command, "status");
  assert.equal(status.target, "rvjob-1");
  assert.equal(status.sessionId, "s1");
  assert.ok(reviewJobHelp().includes("--background"));
});

test("review job client submits, lists, cancels, and attaches", async () => {
  let submitted = null;
  const backend = http.createServer((request, response) => {
    const send = (body) => {
      response.writeHead(200, { "content-type": "application/json" });
      response.end(JSON.stringify(body));
    };
    if (request.url === "/api/v1/reviews/jobs" && request.method === "POST") {
      const chunks = [];
      request.on("data", (chunk) => chunks.push(chunk));
      request.on("end", () => {
        submitted = JSON.parse(Buffer.concat(chunks).toString("utf8"));
        send(job());
      });
      return;
    }
    if (request.url.startsWith("/api/v1/reviews/jobs?session_id=")) {
      send([job({ state: "done", verdict: "approved" })]);
      return;
    }
    if (request.url === "/api/v1/reviews/jobs/rvjob-1") {
      send(job({ state: "done", verdict: "approved", report_path: "docs/review/x.md" }));
      return;
    }
    if (request.url === "/api/v1/reviews/jobs/rvjob-1/cancel") {
      send({ cancelled: true });
      return;
    }
    if (request.url === "/api/v1/reviews/jobs/rvjob-1/attach") {
      send({
        job: job({ state: "done", verdict: "approved", report_path: "docs/review/x.md" }),
        report: "# report\n",
      });
      return;
    }
    response.writeHead(404);
    response.end("{}");
  });
  const port = await listen(backend);
  try {
    const backendUrl = `http://127.0.0.1:${port}`;
    const submittedJob = JSON.parse(
      await runReviewJob({
        backend: backendUrl,
        command: "submit",
        target: path.join(process.cwd(), "docs/review/inbox/x.json"),
        sessionId: "coding",
        json: true,
      }),
    );
    assert.equal(submittedJob.job_id, "rvjob-1");
    assert.equal(submitted.pack, path.join(process.cwd(), "docs/review/inbox/x.json"));
    assert.equal(submitted.session_id, "coding");

    const list = JSON.parse(
      await runReviewJob({
        backend: backendUrl,
        command: "status",
        target: null,
        sessionId: "coding",
        json: true,
      }),
    );
    assert.equal(list.length, 1);
    assert.equal(list[0].state, "done");

    const status = await runReviewJob({
      backend: backendUrl,
      command: "status",
      target: "rvjob-1",
      json: false,
    });
    assert.ok(status.includes("rvjob-1"));
    assert.ok(status.includes("approved"));

    const cancelled = await runReviewJob({
      backend: backendUrl,
      command: "cancel",
      target: "rvjob-1",
      json: false,
    });
    assert.ok(cancelled.includes("cancelled"));

    const attached = await runReviewJob({
      backend: backendUrl,
      command: "attach",
      target: "rvjob-1",
      json: false,
    });
    assert.ok(attached.includes("# report"));
  } finally {
    await close(backend);
  }
});
