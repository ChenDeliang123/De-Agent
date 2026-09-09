import fs from "node:fs";
import path from "node:path";

const runtimeDir =
  process.env.AIAGENT_RUNTIME_DIR || path.resolve(process.cwd(), ".runtime");
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
