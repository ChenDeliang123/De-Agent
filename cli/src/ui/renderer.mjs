import { clipToWidth, displayWidth, visibleWidth, wrapVisible } from "./text.mjs";
import { createMarkdownRenderer } from "./markdown.mjs";
import { detectColorMode, hyperlink, makeStyler, pickSymbols } from "./theme.mjs";

const MaxDisplayText = 4000;
const IDLE_DELAY_MS = 500;
const SPINNER_FRAMES = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"];
const SPINNER_INTERVAL_MS = 100;
const TOOL_PREVIEW_LINES = 8;
// 单行超过该长度就先按普通段落刷出，避免长行长时间不显示。
const MAX_PARTIAL_LINE = 200;

function truncate(text) {
  if (text.length <= MaxDisplayText) {
    return text;
  }
  return `${text.slice(0, MaxDisplayText)}\n… (truncated ${text.length - MaxDisplayText} chars)`;
}

function normalizeArguments(rawArguments) {
  if (typeof rawArguments === "string") {
    try {
      return JSON.parse(rawArguments);
    } catch {
      return rawArguments;
    }
  }
  return rawArguments;
}

function prettyArguments(rawArguments) {
  const args = normalizeArguments(rawArguments);
  if (args === undefined || args === null || args === "") {
    return "";
  }
  if (typeof args === "string") {
    return args;
  }
  try {
    return JSON.stringify(args);
  } catch {
    return String(args);
  }
}

class Spinner {
  #out;
  #styler;
  #region;
  #timer = null;
  #idleTimer = null;
  #frame = 0;
  #label = "";
  #started = 0;
  #active = false;
  #colorName = "accent";

  constructor(out, styler, region = null) {
    this.#out = out;
    this.#styler = styler;
    this.#region = region;
  }

  // 有底部区域时，spinner 渲染成状态行最左侧的一段（不再自己写 \r\x1b[K）。
  #usesRegion() {
    return Boolean(this.#region?.enabled);
  }

  get active() {
    return this.#active;
  }

  start(label, colorName = "accent") {
    if (!this.#out.isTTY) {
      return;
    }
    if (this.#active && this.#label === label) {
      this.#colorName = colorName;
      this.#render();
      return;
    }
    this.#label = label;
    this.#colorName = colorName;
    this.#started = Date.now();
    this.#frame = 0;
    if (!this.#active) {
      this.#active = true;
      this.#timer = setInterval(() => {
        this.#frame = (this.#frame + 1) % SPINNER_FRAMES.length;
        this.#render();
      }, SPINNER_INTERVAL_MS);
      this.#timer.unref?.();
    }
    this.#render();
  }

  schedule(label, colorName = "accent") {
    if (!this.#out.isTTY) {
      return;
    }
    this.cancelScheduled();
    this.#idleTimer = setTimeout(() => {
      this.#idleTimer = null;
      this.start(label, colorName);
    }, IDLE_DELAY_MS);
    this.#idleTimer.unref?.();
  }

  cancelScheduled() {
    if (this.#idleTimer) {
      clearTimeout(this.#idleTimer);
      this.#idleTimer = null;
    }
  }

  stop() {
    this.cancelScheduled();
    if (!this.#active) {
      if (this.#usesRegion()) {
        this.#region.setWorking("");
      }
      return;
    }
    this.#active = false;
    if (this.#timer) {
      clearInterval(this.#timer);
      this.#timer = null;
    }
    if (this.#usesRegion()) {
      this.#region.setWorking("");
      return;
    }
    this.#out.write("\r\x1b[K");
  }

  #render() {
    if (!this.#active) {
      return;
    }
    const seconds = Math.floor((Date.now() - this.#started) / 1000);
    const elapsed = seconds > 0 ? ` ${seconds}s` : "";
    const t = this.#styler;
    const frame = t.fg(this.#colorName, SPINNER_FRAMES[this.#frame]);
    const label = t.dim(`${this.#label}${elapsed}`);
    if (this.#usesRegion()) {
      this.#region.setWorking(`${frame} ${label}`);
      return;
    }
    this.#out.write(`\r\x1b[K${frame} ${label}`);
  }
}

export class TranscriptRenderer {
  #out;
  #t;
  #sym;
  #md;
  #spinner;
  #region;
  #assistantActive = false;
  #assistantWroteText = false;
  #resumeAssistant = false;
  #currentTool = "";
  #turnLabelPrinted = false;
  #mdBuffer = "";
  #cwd = "";

  constructor(out = process.stdout, options = {}) {
    this.#out = out;
    const mode = options.mode ?? detectColorMode(process.env, out);
    this.#t = options.styler ?? makeStyler(mode);
    this.#sym = options.symbols ?? pickSymbols(process.env);
    this.#md = createMarkdownRenderer(this.#t);
    this.#region = options.region ?? null;
    this.#spinner = new Spinner(out, this.#t, this.#region);
  }

  get waiting() {
    return this.#spinner.active;
  }

  setSymbols(symbols) {
    if (symbols) {
      this.#sym = symbols;
    }
  }

  // 当前工作目录，用于把工具里的相对路径解析成可点击的绝对路径。
  setCwd(cwd) {
    this.#cwd = cwd ?? "";
  }

  wait(label = "working") {
    this.#beginOutput();
    this.#spinner.start(label);
  }

  stopWaiting() {
    this.#beginOutput();
  }

  #width() {
    const columns = this.#out.columns;
    return Math.max(20, Number.isFinite(columns) && columns > 0 ? columns : 80);
  }

  user(text) {
    this.#beginOutput();
    this.#flushMdBuffer();
    const t = this.#t;
    const width = this.#width();
    const rows = [];
    for (const line of String(text ?? "").split("\n")) {
      for (const wrapped of wrapVisible(line, Math.max(8, width - 4))) {
        rows.push(wrapped);
      }
    }
    this.#write("\n");
    if (t.mode === "none") {
      // 无色终端（NO_COLOR / 管道）：退化为装订线纯文本，不留空白底色行。
      this.#write(`${t.dim("─".repeat(width))}\n`);
      for (const row of rows) {
        this.#write(`${t.fg("accent", this.#sym.gutter)} ${row}\n`);
      }
    } else {
      // 整块底色「长方体」：上下各留一行空白底色，左右各留 1 列内边距（对齐 pi）。
      const paint = (line) => t.bg("userBg", line);
      this.#write(`${paint(" ".repeat(width))}\n`);
      for (const row of rows) {
        const pad = Math.max(0, width - 2 - visibleWidth(row));
        this.#write(`${paint(`  ${row}${" ".repeat(pad)}`)}\n`);
      }
      this.#write(`${paint(" ".repeat(width))}\n`);
    }
    this.#assistantActive = false;
    this.#assistantWroteText = false;
    this.#resumeAssistant = false;
    this.#turnLabelPrinted = false;
    this.#md.reset();
  }

  startReply() {
    this.#beginOutput();
    this.#flushMdBuffer();
    this.#md.reset();
    this.#write("\n");
    this.#ensureAssistantLabel();
    this.#assistantActive = true;
    this.#assistantWroteText = false;
    this.#resumeAssistant = false;
    this.#spinner.start("thinking", "accent");
  }

  content(token) {
    if (!this.#assistantActive) {
      this.#beginOutput();
      this.#write("\n");
      this.#ensureAssistantLabel();
      this.#assistantActive = true;
    }
    this.#assistantWroteText = true;
    this.#resumeAssistant = false;
    this.#mdBuffer += String(token);
    this.#flushMdLines();
    // 还有未显示的缓存内容时用 spinner 提示正在生成。
    if (this.#mdBuffer !== "") {
      this.#spinner.start("thinking", "accent");
    } else {
      this.#spinner.stop();
    }
  }

  toolStart(id, name, rawArguments) {
    this.#beginOutput();
    this.#flushMdBuffer();
    const t = this.#t;
    this.#write("\n");
    const label = `${t.bold(name)}${id ? ` ${t.dim(`(${id.slice(0, 8)})`)}` : ""}`;
    this.#write(`  ${t.fg("borderMuted", this.#sym.top)} ${label}\n`);
    for (const line of this.#toolCallLines(name, normalizeArguments(rawArguments))) {
      this.#write(`  ${t.fg("borderMuted", this.#sym.bar)} ${line}\n`);
    }
    this.#assistantActive = false;
    this.#assistantWroteText = false;
    this.#resumeAssistant = true;
    this.#currentTool = name;
    this.#spinner.start(`${name} running`, "cyan");
  }

  toolOutput(text) {
    this.#beginOutput();
    for (const line of this.#previewLines(text)) {
      this.#write(`  ${this.#t.fg("borderMuted", this.#sym.bar)} ${this.#t.dim(line)}\n`);
    }
    if (this.#currentTool) {
      this.#spinner.start(`${this.#currentTool} running`, "cyan");
    }
  }

  toolEnd(ok, text) {
    this.#beginOutput();
    const t = this.#t;
    const colorName = ok ? "success" : "error";
    const body = String(text ?? "");
    if (body) {
      for (const line of this.#previewLines(body)) {
        this.#write(`  ${t.fg("borderMuted", this.#sym.bar)} ${t.dim(line)}\n`);
      }
    }
    const status = ok
      ? `${t.fg(colorName, this.#sym.ok)} ok`
      : `${t.fg(colorName, this.#sym.fail)} failed`;
    this.#write(`  ${t.fg(colorName, this.#sym.bottom)} ${status}\n`);
    this.#resumeAssistant = true;
    this.#currentTool = "";
    this.#spinner.start("thinking", "accent");
  }

  endReply() {
    this.#beginOutput();
    this.#flushMdBuffer();
    this.#write("\n");
    this.#assistantActive = false;
    this.#assistantWroteText = false;
    this.#resumeAssistant = false;
    this.#turnLabelPrinted = false;
  }

  error(message) {
    this.#beginOutput();
    this.#flushMdBuffer();
    const t = this.#t;
    this.#write(`\n${t.fg("error", `error: ${message}`)}\n`);
    this.endReply();
  }

  interrupted() {
    this.#beginOutput();
    this.#flushMdBuffer();
    const t = this.#t;
    this.#write(`\n${t.fg("warning", `${this.#sym.interrupted} interrupted`)}\n`);
    this.#assistantActive = false;
    this.#assistantWroteText = false;
    this.#resumeAssistant = false;
  }

  approvalRequest(data) {
    this.#beginOutput();
    this.#flushMdBuffer();
    const t = this.#t;
    const name = data?.name ?? "tool";
    const reason = data?.reason ?? "requires approval";
    const args = prettyArguments(data?.arguments);
    this.#write(`\n${t.fg("warning", `${this.#sym.warn} approval needed`)} ${t.bold(name)}`);
    if (args) {
      this.#write(` ${t.dim(clipToWidth(args, Math.max(10, this.#width() - 40)))}`);
    }
    this.#write(`\n${t.dim(`  ${reason}`)}\n`);
    this.#assistantActive = false;
    this.#assistantWroteText = false;
    this.#resumeAssistant = true;
  }

  approvalResolved(decision) {
    this.#beginOutput();
    const t = this.#t;
    const label =
      decision === "allow_once"
        ? t.fg("success", "approved (once)")
        : decision === "allow_session"
          ? t.fg("success", "approved (this session)")
          : t.fg("error", "denied");
    this.#write(`${t.dim("  →")} ${label}\n`);
    this.#resumeAssistant = true;
    this.#spinner.start("thinking", "accent");
  }

  compaction(data) {
    this.#beginOutput();
    this.#flushMdBuffer();
    const t = this.#t;
    const before = data?.tokens_before ?? "?";
    const after = data?.tokens_after ?? "?";
    const reason = data?.forced ? " (overflow)" : "";
    this.#write(
      `\n${t.dim(`${this.#sym.compact} context compacted${reason}: ${before} → ${after} tokens`)}\n`,
    );
    this.#assistantActive = false;
    this.#assistantWroteText = false;
    this.#resumeAssistant = true;
    this.#spinner.start("thinking", "accent");
  }

  #ensureAssistantLabel() {
    if (this.#turnLabelPrinted) {
      return;
    }
    this.#write(`${this.#t.dim("de agent")}\n`);
    this.#turnLabelPrinted = true;
  }

  // 把已收到的完整行交给 markdown 渲染；过长的不满行也先刷出。
  #flushMdLines() {
    let index;
    while ((index = this.#mdBuffer.indexOf("\n")) !== -1) {
      const line = this.#mdBuffer.slice(0, index);
      this.#mdBuffer = this.#mdBuffer.slice(index + 1);
      this.#beginOutput();
      this.#writeMdLine(line);
    }
    if (displayWidth(this.#mdBuffer) > MAX_PARTIAL_LINE) {
      this.#beginOutput();
      this.#writeMdLine(this.#mdBuffer);
      this.#mdBuffer = "";
    }
  }

  #flushMdBuffer() {
    if (this.#mdBuffer !== "") {
      this.#writeMdLine(this.#mdBuffer);
      this.#mdBuffer = "";
    }
  }

  #writeMdLine(line) {
    for (const rendered of this.#md.renderLine(line)) {
      this.#write(rendered.length ? `  ${rendered}\n` : "\n");
    }
  }

  #toolCallLines(name, args) {
    const t = this.#t;
    const lines = [];
    const hasPath =
      args && typeof args === "object" && typeof args.path === "string" && args.path;
    if (hasPath) {
      lines.push(t.dim(this.#pathLink(args.path)));
    }
    if (
      name === "edit" &&
      args &&
      typeof args === "object" &&
      (args.oldText !== undefined ||
        args.newText !== undefined ||
        Array.isArray(args.edits))
    ) {
      // 一次调用可能带多个块（edits[]，对齐 pi）：逐块渲染 -/+，块多时标出序号。
      const blocks = Array.isArray(args.edits)
        ? args.edits.filter((block) => block && typeof block === "object")
        : [{ oldText: args.oldText, newText: args.newText }];
      blocks.forEach((block, index) => {
        if (blocks.length > 1) {
          lines.push(t.dim(`block ${index + 1}/${blocks.length}`));
        }
        for (const line of String(block.oldText ?? "").split("\n")) {
          if (line !== "") {
            lines.push(t.fg("diffRemoved", `- ${line}`));
          }
        }
        for (const line of String(block.newText ?? "").split("\n")) {
          if (line !== "") {
            lines.push(t.fg("diffAdded", `+ ${line}`));
          }
        }
      });
      const max = 24;
      if (lines.length > max) {
        return lines.slice(0, max).concat([t.dim(`… (+${lines.length - max} lines)`)]);
      }
      return lines;
    }
    // 其余工具：去掉已单独展示的 path，剩下的参数紧凑打印。
    let rest = args;
    if (hasPath) {
      rest = { ...args };
      delete rest.path;
    }
    const text = prettyArguments(rest);
    if (text && text !== "{}") {
      lines.push(t.dim(clipToWidth(text, this.#width() - 6)));
    }
    return lines;
  }

  // 把工具参数里的路径变成 OSC 8 可点击链接（仅 TTY）。
  #pathLink(path) {
    const text = String(path);
    if (!this.#out.isTTY) {
      return text;
    }
    const absolute = text.startsWith("/")
      ? text
      : this.#cwd
        ? `${this.#cwd}/${text}`
        : "";
    if (!absolute) {
      return text;
    }
    return hyperlink(`file://${absolute}`, text);
  }

  #previewLines(text) {
    const maxWidth = Math.max(10, this.#width() - 6);
    const lines = truncate(String(text ?? "")).split("\n");
    const shown = lines.slice(0, TOOL_PREVIEW_LINES).map((line) => clipToWidth(line, maxWidth));
    if (lines.length > TOOL_PREVIEW_LINES) {
      shown.push(`… (+${lines.length - TOOL_PREVIEW_LINES} lines)`);
    }
    return shown;
  }

  #beginOutput() {
    this.#spinner.stop();
  }

  #write(text) {
    if (this.#region?.enabled) {
      this.#region.appendOutput(text);
      return;
    }
    this.#out.write(text);
  }
}

// 供测试/状态栏复用。
export { displayWidth };
