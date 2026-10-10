import { clipToWidth, displayWidth } from "./editor.mjs";
import { detectColorMode, makeStyler, pickSymbols } from "./theme.mjs";

const MaxDisplayText = 4000;
const IDLE_DELAY_MS = 500;
const SPINNER_FRAMES = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"];
const SPINNER_INTERVAL_MS = 100;
const TOOL_PREVIEW_LINES = 8;

function truncate(text) {
  if (text.length <= MaxDisplayText) {
    return text;
  }
  return `${text.slice(0, MaxDisplayText)}\n… (truncated ${text.length - MaxDisplayText} chars)`;
}

function prettyArguments(rawArguments) {
  if (rawArguments === undefined || rawArguments === null || rawArguments === "") {
    return "";
  }
  if (typeof rawArguments === "string") {
    try {
      return JSON.stringify(JSON.parse(rawArguments));
    } catch {
      return rawArguments;
    }
  }
  if (typeof rawArguments === "object") {
    return JSON.stringify(rawArguments);
  }
  return String(rawArguments);
}

class Spinner {
  #out;
  #styler;
  #timer = null;
  #idleTimer = null;
  #frame = 0;
  #label = "";
  #started = 0;
  #active = false;
  #colorName = "accent";

  constructor(out, styler) {
    this.#out = out;
    this.#styler = styler;
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
      return;
    }
    this.#active = false;
    if (this.#timer) {
      clearInterval(this.#timer);
      this.#timer = null;
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
    this.#out.write(`\r\x1b[K${frame} ${label}`);
  }
}

export class TranscriptRenderer {
  #out;
  #t;
  #sym;
  #spinner;
  #assistantActive = false;
  #assistantWroteText = false;
  #resumeAssistant = false;
  #currentTool = "";
  #atLineStart = true;
  #turnLabelPrinted = false;

  constructor(out = process.stdout, options = {}) {
    this.#out = out;
    const mode = options.mode ?? detectColorMode(process.env, out);
    this.#t = options.styler ?? makeStyler(mode);
    this.#sym = options.symbols ?? pickSymbols(process.env);
    this.#spinner = new Spinner(out, this.#t);
  }

  get waiting() {
    return this.#spinner.active;
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
    const t = this.#t;
    this.#write(`\n${t.dim("─".repeat(this.#width()))}\n`);
    this.#write(`${t.fg("accent", this.#sym.gutter)} ${text}\n`);
    this.#assistantActive = false;
    this.#assistantWroteText = false;
    this.#resumeAssistant = false;
    this.#atLineStart = true;
    this.#turnLabelPrinted = false;
  }

  startReply() {
    this.#beginOutput();
    this.#write("\n");
    this.#ensureAssistantLabel();
    this.#assistantActive = true;
    this.#assistantWroteText = false;
    this.#resumeAssistant = false;
    this.#atLineStart = true;
    this.#spinner.start("thinking", "accent");
  }

  content(token) {
    this.#beginOutput();
    if (!this.#assistantActive) {
      this.#write("\n");
      this.#ensureAssistantLabel();
      this.#assistantActive = true;
      this.#atLineStart = true;
    }
    this.#assistantWroteText = true;
    this.#resumeAssistant = false;
    this.#writeIndented(token);
    this.#spinner.schedule("thinking", "accent");
  }

  toolStart(id, name, rawArguments) {
    this.#beginOutput();
    const t = this.#t;
    this.#write("\n");
    const label = `${t.bold(name)}${id ? ` ${t.dim(`(${id.slice(0, 8)})`)}` : ""}`;
    this.#write(`  ${t.fg("borderMuted", this.#sym.top)} ${label}\n`);
    const args = prettyArguments(rawArguments);
    if (args) {
      const clipped = clipToWidth(args, this.#width() - 6);
      this.#write(`  ${t.fg("borderMuted", this.#sym.bar)} ${t.dim(clipped)}\n`);
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
    const status = ok ? `${t.fg(colorName, this.#sym.ok)} ok` : `${t.fg(colorName, this.#sym.fail)} failed`;
    this.#write(`  ${t.fg(colorName, this.#sym.bottom)} ${status}\n`);
    this.#resumeAssistant = true;
    this.#currentTool = "";
    this.#atLineStart = true;
    this.#spinner.start("thinking", "accent");
  }

  endReply() {
    this.#beginOutput();
    this.#write("\n");
    this.#assistantActive = false;
    this.#assistantWroteText = false;
    this.#resumeAssistant = false;
    this.#atLineStart = true;
    this.#turnLabelPrinted = false;
  }

  error(message) {
    this.#beginOutput();
    const t = this.#t;
    this.#write(`\n${t.fg("error", `error: ${message}`)}\n`);
    this.endReply();
  }

  interrupted() {
    this.#beginOutput();
    const t = this.#t;
    this.#write(`\n${t.fg("warning", `${this.#sym.interrupted} interrupted`)}\n`);
    this.#assistantActive = false;
    this.#assistantWroteText = false;
    this.#resumeAssistant = false;
    this.#atLineStart = true;
  }

  approvalRequest(data) {
    this.#beginOutput();
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
    this.#atLineStart = true;
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
    this.#atLineStart = true;
    this.#spinner.start("thinking", "accent");
  }

  compaction(data) {
    this.#beginOutput();
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
    this.#atLineStart = true;
    this.#spinner.start("thinking", "accent");
  }

  #ensureAssistantLabel() {
    if (this.#turnLabelPrinted) {
      return;
    }
    this.#write(`${this.#t.dim("de agent")}\n`);
    this.#turnLabelPrinted = true;
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

  #writeIndented(text) {
    const pieces = String(text).split("\n");
    for (let i = 0; i < pieces.length; i += 1) {
      if (i > 0) {
        this.#write("\n");
        this.#atLineStart = true;
      }
      if (pieces[i].length === 0) {
        continue;
      }
      if (this.#atLineStart) {
        this.#write("  ");
        this.#atLineStart = false;
      }
      this.#write(pieces[i]);
    }
  }

  #beginOutput() {
    this.#spinner.stop();
  }

  #write(text) {
    this.#out.write(text);
  }
}

// 供测试/状态栏复用。
export { displayWidth };
