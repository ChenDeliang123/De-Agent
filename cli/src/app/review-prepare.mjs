// `de review prepare` / `/review prepare` 的共享实现：
// 只做参数 → POST /api/v1/reviews → 打印结果，不进入交互 CLI。
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { createReview, normalizeBackendUrl } from "./session.mjs";

export function parseReviewPrepareArgs(argv) {
  const options = {
    backend: "http://127.0.0.1:18081",
    cwd: process.cwd(),
    feature: null,
    base: "HEAD",
    request: null,
    requestFile: null,
    contextPaths: [],
    verificationFile: null,
    agent: null,
    producer: "external",
    previousReport: null,
    json: false,
  };
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === "--backend" && i + 1 < argv.length) {
      options.backend = argv[++i];
    } else if (arg === "--cwd" && i + 1 < argv.length) {
      options.cwd = path.resolve(argv[++i]);
    } else if (arg === "--feature" && i + 1 < argv.length) {
      options.feature = argv[++i];
    } else if (arg === "--base" && i + 1 < argv.length) {
      options.base = argv[++i];
    } else if (arg === "--request" && i + 1 < argv.length) {
      options.request = argv[++i];
    } else if (arg === "--request-file" && i + 1 < argv.length) {
      options.requestFile = argv[++i];
    } else if (arg === "--context" && i + 1 < argv.length) {
      options.contextPaths.push(argv[++i]);
    } else if (arg === "--verification-file" && i + 1 < argv.length) {
      options.verificationFile = argv[++i];
    } else if (arg === "--agent" && i + 1 < argv.length) {
      options.agent = argv[++i];
    } else if (arg === "--producer" && i + 1 < argv.length) {
      options.producer = argv[++i];
    } else if (arg === "--previous-report" && i + 1 < argv.length) {
      options.previousReport = argv[++i];
    } else if (arg === "--json") {
      options.json = true;
    } else if (arg === "--help" || arg === "-h") {
      options.help = true;
    } else if (!arg.startsWith("-") && !options.feature) {
      options.feature = arg;
    }
  }
  return options;
}

export function reviewPrepareHelp() {
  return [
    "usage: de review prepare --feature <name> [options]",
    "",
    "options:",
    "  --base <ref>                 base git revision (default HEAD)",
    "  --request <text>             inline review request (mutually exclusive with --request-file)",
    "  --request-file <path>        request markdown inside the repository",
    "  --context <path>             context document (repeatable)",
    "  --verification-file <json>   declared argv verification array",
    "  --agent <name>               producer agent name (claude-code, codex, ci, ...)",
    "  --producer <kind>            external | manual | de-session (default external)",
    "  --previous-report <path>     previous review report (round 2)",
    "  --json                       print machine-readable JSON",
    "  --cwd <dir>                  repository working directory (default current)",
    "  --backend <url>              backend base url (default http://127.0.0.1:18081)",
  ].join("\n");
}

export async function reviewPrepare(options) {
  if (!options.feature) {
    throw new Error("--feature is required");
  }
  if (options.request && options.requestFile) {
    throw new Error("--request and --request-file are mutually exclusive");
  }
  let verification = [];
  if (options.verificationFile) {
    const text = fs.readFileSync(options.verificationFile, "utf8");
    const parsed = JSON.parse(text);
    if (!Array.isArray(parsed)) {
      throw new Error("verification file must contain a JSON array");
    }
    verification = parsed;
  }
  const payload = {
    cwd: options.cwd,
    feature: options.feature,
    base: options.base,
    producer: options.producer,
    context_paths: options.contextPaths,
    verification,
  };
  if (options.agent) {
    payload.agent = options.agent;
  }
  if (options.request) {
    payload.request_text = options.request;
  }
  if (options.requestFile) {
    payload.request_file = options.requestFile;
  }
  if (options.previousReport) {
    payload.previous_report = options.previousReport;
  }
  if (options.sessionId) {
    payload.session_id = options.sessionId;
  }
  return createReview(normalizeBackendUrl(options.backend), payload);
}

export function formatReviewResult(result) {
  return [
    `review_id: ${result.review_id ?? ""}`,
    `pack: ${result.pack_path ?? ""}`,
    `report: ${result.report_path_hint ?? ""}`,
    `changed_files: ${result.changed_files ?? 0}  diff_bytes: ${result.diff_bytes ?? 0}` +
      (result.truncated ? " (truncated)" : ""),
    `intent_source: ${result.intent_source ?? ""}` +
      (result.context_missing ? " (context_missing)" : ""),
    "",
    `next: de review ${result.pack_path ?? "<pack>"}`,
  ].join("\n");
}

async function main() {
  const options = parseReviewPrepareArgs(process.argv.slice(2));
  if (options.help) {
    console.log(reviewPrepareHelp());
    return;
  }
  try {
    const result = await reviewPrepare(options);
    if (options.json) {
      console.log(JSON.stringify(result));
    } else {
      console.log(formatReviewResult(result));
    }
  } catch (error) {
    console.error(`de review prepare: ${error.message}`);
    process.exitCode = 1;
  }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main();
}
