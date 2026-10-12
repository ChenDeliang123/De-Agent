import fs from "node:fs";
import path from "node:path";

import { resolveDataDir } from "./config.mjs";

// 日志目录优先级：$AIAGENT_RUNTIME_DIR > <data_dir>/runtime > cwd 下的 .runtime。
// 安装态下 de 会显式传入 AIAGENT_RUNTIME_DIR，这里的主要目的是让直接运行
// `node cli.mjs` 时不污染 cwd。
function defaultRuntimeDir(env = process.env) {
  const dataDir = resolveDataDir(env);
  if (dataDir) {
    return path.join(dataDir, "runtime");
  }
  return path.resolve(process.cwd(), ".runtime");
}

const runtimeDir =
  process.env.AIAGENT_RUNTIME_DIR || defaultRuntimeDir();
const logFile =
  process.env.AIAGENT_CLI_LOG || path.join(runtimeDir, "cli.log");

try {
  fs.mkdirSync(runtimeDir, { recursive: true });
} catch {
  // 日志失败不应影响 CLI 主流程。
}

export function log(level, moduleName, message) {
  const line = `${new Date().toISOString()} ${level} [${moduleName}] ${message}\n`;
  try {
    fs.appendFileSync(logFile, line, "utf8");
  } catch {
    // 文件不可写时静默。
  }
}
