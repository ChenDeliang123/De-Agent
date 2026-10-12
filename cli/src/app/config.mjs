// CLI 设置文件（可选）。
//
// 目前只用于持久化 Ctrl+G 的外部编辑器命令，格式与后端 conf 保持一致的 INI：
//
//   [editor]
//   external = code --wait
//
// 查找顺序：
//   --cli-config <file> > $AIAGENT_CLI_CONFIG > ./conf/cli.ini > <repo>/conf/cli.ini
//   > <data_dir>/conf/cli.ini > $AIAGENT_SHARE_DIR/conf/cli.ini
// 其中 data_dir 与后端一致（$DE_HOME > $XDG_DATA_HOME/de > ~/.de）。
// 找不到文件不报错，返回空配置。

import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

// 本文件位于 cli/src/app/：CLI 包根目录是它的上两级。
const CLI_DIR = join(dirname(fileURLToPath(import.meta.url)), "..", "..");

// 与后端 ResolveAppPaths 保持一致的用户数据目录解析。
// 无法确定时返回 null，调用方自行回退。
export function resolveDataDir(env = process.env) {
  if (typeof env.DE_HOME === "string" && env.DE_HOME.trim() !== "") {
    return env.DE_HOME.trim();
  }
  if (typeof env.XDG_DATA_HOME === "string" && env.XDG_DATA_HOME.trim() !== "") {
    return join(env.XDG_DATA_HOME.trim(), "de");
  }
  if (typeof env.HOME === "string" && env.HOME.trim() !== "") {
    return join(env.HOME.trim(), ".de");
  }
  return null;
}

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
  const dataDir = resolveDataDir(env);
  if (dataDir) {
    candidates.push(join(dataDir, "conf", "cli.ini"));
  }
  if (typeof env.AIAGENT_SHARE_DIR === "string" && env.AIAGENT_SHARE_DIR.trim() !== "") {
    candidates.push(join(env.AIAGENT_SHARE_DIR.trim(), "conf", "cli.ini"));
  }
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
    return { path: null, externalEditor: null, theme: null, nerdFont: false };
  }
  let content;
  try {
    content = readFileSync(path, "utf-8");
  } catch (error) {
    return { path, externalEditor: null, theme: null, nerdFont: false, error: error.message };
  }
  const ini = parseIni(content);
  const editor = ini.editor ?? {};
  const ui = ini.ui ?? {};
  const externalEditor = editor.external || editor.external_editor || null;
  const theme = ui.theme || null;
  const nerdFont =
    ui.nerd_font === "1" || String(ui.nerd_font || "").toLowerCase() === "true";
  return { path, externalEditor, theme, nerdFont };
}

// 在 INI 文件中新增/更新 [ui] 段下的若干键（用于 /theme 持久化）。
// 保留文件其余内容；找不到 [ui] 段时追加。
export function updateCliUiConfig(path, updates) {
  if (!path) {
    return false;
  }
  let lines = [];
  try {
    lines = readFileSync(path, "utf-8").split(/\r?\n/);
  } catch {
    lines = [];
  }
  // 去掉末尾空行，便于追加。
  while (lines.length > 0 && lines[lines.length - 1].trim() === "") {
    lines.pop();
  }

  let sectionStart = -1;
  let sectionEnd = lines.length;
  for (let i = 0; i < lines.length; i += 1) {
    const trimmed = lines[i].trim();
    if (trimmed.startsWith("[") && trimmed.endsWith("]")) {
      const name = trimmed.slice(1, -1).trim().toLowerCase();
      if (sectionStart === -1 && name === "ui") {
        sectionStart = i;
      } else if (sectionStart !== -1) {
        sectionEnd = i;
        break;
      }
    }
  }

  if (sectionStart === -1) {
    if (lines.length > 0) {
      lines.push("");
    }
    lines.push("[ui]");
    for (const [key, value] of Object.entries(updates)) {
      lines.push(`${key} = ${value}`);
    }
  } else {
    for (const [key, value] of Object.entries(updates)) {
      let found = false;
      for (let i = sectionStart + 1; i < sectionEnd; i += 1) {
        const trimmed = lines[i].trim();
        if (!trimmed || trimmed.startsWith("#") || trimmed.startsWith(";")) {
          continue;
        }
        const equals = trimmed.indexOf("=");
        if (equals === -1) {
          continue;
        }
        if (trimmed.slice(0, equals).trim().toLowerCase() === key.toLowerCase()) {
          lines[i] = `${key} = ${value}`;
          found = true;
          break;
        }
      }
      if (!found) {
        lines.splice(sectionEnd, 0, `${key} = ${value}`);
        sectionEnd += 1;
      }
    }
  }

  writeFileSync(path, `${lines.join("\n")}\n`, "utf-8");
  return true;
}
