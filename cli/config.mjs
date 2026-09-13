// CLI 设置文件（可选）。
//
// 目前只用于持久化 Ctrl+G 的外部编辑器命令，格式与后端 conf 保持一致的 INI：
//
//   [editor]
//   external = code --wait
//
// 查找顺序：
//   --cli-config <file> > $AIAGENT_CLI_CONFIG > ./conf/cli.ini > <cli>/../conf/cli.ini
// 找不到文件不报错，返回空配置。

import { existsSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const CLI_DIR = dirname(fileURLToPath(import.meta.url));

export function parseIni(content) {
  const result = {};
  let section = "";
  for (const rawLine of String(content).split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line || line.startsWith("#") || line.startsWith(";")) {
      continue;
    }
    if (line.startsWith("[") && line.endsWith("]")) {
      section = line.slice(1, -1).trim().toLowerCase();
      continue;
    }
    const equals = line.indexOf("=");
    if (equals === -1) {
      continue;
    }
    const key = line.slice(0, equals).trim().toLowerCase();
    const value = line.slice(equals + 1).trim();
    if (!section || !key) {
      continue;
    }
    result[section] = result[section] ?? {};
    result[section][key] = value;
  }
  return result;
}

export function resolveCliConfigPath({
  explicit,
  env = process.env,
  cwd = process.cwd(),
  cliDir = CLI_DIR,
} = {}) {
  if (typeof explicit === "string" && explicit.trim() !== "") {
    return explicit.trim();
  }
  if (typeof env.AIAGENT_CLI_CONFIG === "string" && env.AIAGENT_CLI_CONFIG.trim() !== "") {
    return env.AIAGENT_CLI_CONFIG.trim();
  }
  const candidates = [join(cwd, "conf", "cli.ini"), join(cliDir, "..", "conf", "cli.ini")];
  for (const candidate of candidates) {
    if (existsSync(candidate)) {
      return candidate;
    }
  }
  return null;
}

export function loadCliConfig(options = {}) {
  const path = resolveCliConfigPath(options);
  if (!path || !existsSync(path)) {
    return { path: null, externalEditor: null };
  }
  let content;
  try {
    content = readFileSync(path, "utf-8");
  } catch (error) {
    return { path, externalEditor: null, error: error.message };
  }
  const editor = parseIni(content).editor ?? {};
  const externalEditor = editor.external || editor.external_editor || null;
  return { path, externalEditor };
}
