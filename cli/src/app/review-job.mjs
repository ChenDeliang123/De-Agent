// `de review --background/status/cancel/attach` 的共享实现。
// 只做 HTTP 调用与文本输出，不进入交互 CLI。
import path from "node:path";
import { fileURLToPath } from "node:url";

import {
  attachReviewJob,
  cancelReviewJob,
  getReviewJob,
  listReviewJobs,
  normalizeBackendUrl,
  submitReviewJob,
} from "./session.mjs";

export function parseReviewJobArgs(argv) {
  const options = {
    backend: "http://127.0.0.1:18081",
    sessionId: null,
    json: false,
    command: null,
    target: null,
  };
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === "--backend" && i + 1 < argv.length) {
      options.backend = argv[++i];
    } else if (arg === "--session" && i + 1 < argv.length) {
      options.sessionId = argv[++i];
    } else if (arg === "--json") {
      options.json = true;
    } else if (!arg.startsWith("-") && !options.command) {
      options.command = arg;
    } else if (!arg.startsWith("-") && !options.target) {
      options.target = arg;
    }
  }
  return options;
}

export function reviewJobHelp() {
  return [
    "usage: de review --background <pack> [--session <id>] [--json]",
    "       de review status [job_id] [--json]",
    "       de review cancel <job_id>",
    "       de review attach <job_id>",
    "",
    "background review runs in a separate reviewer session; the original",
    "session can keep working. status/attach do not block the job.",
  ].join("\n");
}

function formatJob(job) {
  const progress = job.progress ?? {};
  return [
    `${job.job_id}  ${job.state}  ${job.feature ?? ""}@${job.branch ?? ""}`.trimEnd(),
    `  report: ${job.report_path || "-"}`,
    `  verdict: ${job.verdict || "-"}  findings: ${progress.findings ?? 0}`,
    `  progress: ${progress.files_done ?? 0}/${progress.files_total ?? 0}` +
      (progress.current_command ? `  command: ${progress.current_command}` : ""),
    job.error ? `  error: ${job.error}` : null,
  ]
    .filter(Boolean)
    .join("\n");
}

export async function runReviewJob(options) {
  const backend = normalizeBackendUrl(options.backend);
  if (options.command === "submit") {
    const pack = path.resolve(options.target);
    const job = await submitReviewJob(backend, pack, options.sessionId ?? undefined);
    return options.json ? JSON.stringify(job) : formatJob(job);
  }
  if (options.command === "status" || options.command === "jobs") {
    const jobs = options.target
      ? [await getReviewJob(backend, options.target)]
      : await listReviewJobs(backend, options.sessionId ?? undefined);
    if (options.json) {
      return JSON.stringify(jobs);
    }
    if (jobs.length === 0) {
      return "no review jobs";
    }
    return jobs.map(formatJob).join("\n\n");
  }
  if (options.command === "cancel") {
    if (!options.target) {
      throw new Error("cancel requires a job id");
    }
    await cancelReviewJob(backend, options.target);
    return options.json
      ? JSON.stringify({ cancelled: true, job_id: options.target })
      : `cancelled ${options.target}`;
  }
  if (options.command === "attach") {
    if (!options.target) {
      throw new Error("attach requires a job id");
    }
    const result = await attachReviewJob(backend, options.target);
    if (options.json) {
      return JSON.stringify(result);
    }
    const report = result.report ? `\n\n${result.report}` : "";
    return `${formatJob(result.job)}${report}`;
  }
  throw new Error(`unknown review job command: ${options.command ?? "(none)"}`);
}

async function main() {
  // `de review --background <pack>` 由启动器拼成 `submit <pack>`。
  const options = parseReviewJobArgs(process.argv.slice(2));
  if (options.command === "help" || options.command === "--help") {
    console.log(reviewJobHelp());
    return;
  }
  try {
    const output = await runReviewJob(options);
    console.log(output);
  } catch (error) {
    console.error(`de review: ${error.message}`);
    process.exitCode = 1;
  }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main();
}
