import {
  clipToWidth,
  clipVisible,
  displayWidth,
  visibleWidth,
  wrapVisible,
} from "./text.mjs";
import { createMarkdownRenderer } from "./markdown.mjs";
import { detectColorMode, hyperlink, makeStyler, pickSymbols } from "./theme.mjs";

const MaxDisplayText = 4000;
const IDLE_DELAY_MS = 500;
const SPINNER_FRAMES = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"];
const SPINNER_INTERVAL_MS = 100;
const TOOL_PREVIEW_LINES = 8;
// 后台任务（subagent_result）补打报告摘要时最多显示的行数，余下折叠。
const MaxResultLines = 5;
// 单行超过该长度就先按普通段落刷出，避免长行长时间不显示。
const MAX_PARTIAL_LINE = 200;
// 根 `task` 工具结果最多打印的行数；超出折叠，模型仍收到完整结果。
const MAX_TASK_RESULT_LINES = 80;
// 子 Agent 事后 transcript：每 Agent 保留的行数与单行字符上限（仅内存）。
const MAX_TRANSCRIPT_LINES = 200;
const MAX_TRANSCRIPT_LINE_CHARS = 500;
// 底部状态区最多同时显示的 Agent 行数；超出显示 `… +N more`。
const MAX_SUBAGENT_STATUS_ROWS = 4;

function truncate(text) {
  if (text.length <= MaxDisplayText) {
    return text;
  }
  return `${text.slice(0, MaxDisplayText)}\n… (truncated ${text.length - MaxDisplayText} chars)`;
}

// 子 Agent 耗时：1 分钟内用一位小数秒（3.9s），更长用 m/s。
function formatDuration(ms) {
  const value = Number(ms);
  if (!Number.isFinite(value) || value < 0) {
    return "";
  }
  if (value < 60_000) {
    return `${(value / 1000).toFixed(1)}s`;
  }
  const minutes = Math.floor(value / 60_000);
  const seconds = Math.round((value % 60_000) / 1000);
  return `${minutes}m ${seconds}s`;
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
  // 文本块归属：子 Agent 的块只在首行加一次 [agent] 标注。
  #contentAgent = "";
  #pendingAgentPrefix = "";
  // 前台子 Agent 的实时状态：agentId -> { state, tool, toolCalls, failed }。
  #subagents = new Map();
  // 事后 transcript：agentId -> { lines, dropped, partial }；仅内存、有界。
  #subagentTranscripts = new Map();
  // 根工具名：按工具调用 id 记录，用于识别 `task` 并打印完整报告。
  #rootToolNames = new Map();
  #rootToolName = "";

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

  // 当前回合有 transcript 的 Agent id（供 `/agents show` 使用）。
  subagentTranscriptIds() {
    return [...this.#subagentTranscripts.entries()]
      .filter(([, entry]) => entry.lines.length > 0 || entry.partial !== "")
      .map(([agent]) => agent);
  }

  // 读取某个 Agent 的有界 transcript；不存在/为空返回 null。
  subagentTranscript(agentId) {
    const entry = this.#subagentTranscripts.get(String(agentId ?? ""));
    if (!entry) {
      return null;
    }
    const lines = [...entry.lines];
    if (entry.partial !== "") {
      lines.push(this.#truncateTranscriptLine(entry.partial));
    }
    if (lines.length === 0) {
      return null;
    }
    return { lines, dropped: entry.dropped, truncated: entry.dropped > 0 };
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

  // 子 Agent 的行（带标注/缩进）：写之前按可见宽度裁剪，避免越界。
  #writeClipped(text) {
    const width = this.#width();
    if (visibleWidth(text) <= width) {
      this.#write(`${text}\n`);
      return;
    }
    // clipVisible 的省略号额外占一列，所以按 width - 1 裁剪，整行不越界。
    this.#write(`${clipVisible(text, width - 1)}\n`);
  }

  // 按事件归属写一行：子 Agent 走裁剪路径，根 Agent（agent 为空）保持原有输出。
  #writeRow(agent, text) {
    if (agent) {
      this.#writeClipped(text);
      return;
    }
    this.#write(`${text}\n`);
  }

  // 子 Agent 的 dim 标注，形如 `[explorer#1] `。
  #agentTag(agent) {
    return this.#t.dim(`[${agent}] `);
  }

  // 取 agent 名：接受 id 字符串，也接受带 agent_id 的整帧 data。
  #agentName(value) {
    if (value && typeof value === "object") {
      return String(value.agent_id ?? "");
    }
    return String(value ?? "");
  }

  // 把前台子 Agent 状态同步到底部区域；没有 region 或旧 region 时静默跳过。
  #syncSubagentStatus() {
    if (typeof this.#region?.setSubagentRows !== "function") {
      return;
    }
    const entries = [...this.#subagents.entries()];
    const rows = [];
    for (const [agent, state] of entries.slice(0, MAX_SUBAGENT_STATUS_ROWS)) {
      rows.push(this.#formatSubagentStatusRow(agent, state));
    }
    if (entries.length > MAX_SUBAGENT_STATUS_ROWS) {
      rows.push(this.#t.dim(`  … +${entries.length - MAX_SUBAGENT_STATUS_ROWS} more`));
    }
    this.#region.setSubagentRows(rows);
  }

  #formatSubagentStatusRow(agent, state) {
    const parts = [agent];
    if (state.state === "tool" && state.tool) {
      parts.push(state.tool);
    } else if (state.state === "tool") {
      parts.push("tool");
    } else if (state.state) {
      parts.push(state.state);
    }
    const calls = state.toolCalls ?? 0;
    parts.push(`${calls} call${calls === 1 ? "" : "s"}`);
    if ((state.failed ?? 0) > 0) {
      parts.push(`${state.failed} failed`);
    }
    return this.#t.dim(`  ${this.#sym.subagent} ${parts.join(` ${this.#sym.dot} `)}`);
  }

  #updateSubagent(agentId, patch) {
    if (!agentId) {
      return;
    }
    const current = this.#subagents.get(agentId) ?? {
      state: "starting",
      tool: "",
      toolCalls: 0,
      failed: 0,
    };
    this.#subagents.set(agentId, { ...current, ...patch });
    this.#syncSubagentStatus();
  }

  // 回合边界：清状态行；clearTranscripts=true 时同时丢弃上一回合的事后缓冲。
  #resetSubagentTracking(clearTranscripts) {
    this.#subagents.clear();
    this.#rootToolNames.clear();
    this.#rootToolName = "";
    if (clearTranscripts) {
      this.#subagentTranscripts.clear();
    }
    this.#syncSubagentStatus();
  }

  #ensureTranscript(agentId) {
    let entry = this.#subagentTranscripts.get(agentId);
    if (!entry) {
      entry = { lines: [], dropped: 0, partial: "" };
      this.#subagentTranscripts.set(agentId, entry);
    }
    return entry;
  }

  #truncateTranscriptLine(line) {
    const source = String(line ?? "").replace(/\r$/, "");
    return source.length > MAX_TRANSCRIPT_LINE_CHARS
      ? `${source.slice(0, MAX_TRANSCRIPT_LINE_CHARS - 1)}…`
      : source;
  }

  #pushTranscriptLine(entry, line) {
    entry.lines.push(this.#truncateTranscriptLine(line));
    if (entry.lines.length > MAX_TRANSCRIPT_LINES) {
      entry.lines.shift();
      entry.dropped += 1;
    }
  }

  #appendTranscriptLines(agentId, lines) {
    if (!agentId || !Array.isArray(lines) || lines.length === 0) {
      return;
    }
    const entry = this.#ensureTranscript(agentId);
    for (const line of lines) {
      this.#pushTranscriptLine(entry, line);
    }
  }

  #appendTranscriptContent(agentId, token) {
    if (!agentId) {
      return;
    }
    const entry = this.#ensureTranscript(agentId);
    entry.partial += String(token ?? "");
    let index;
    while ((index = entry.partial.indexOf("\n")) !== -1) {
      const line = entry.partial.slice(0, index);
      entry.partial = entry.partial.slice(index + 1);
      this.#pushTranscriptLine(entry, line);
    }
    if (entry.partial.length > MAX_TRANSCRIPT_LINE_CHARS) {
      this.#pushTranscriptLine(entry, entry.partial);
      entry.partial = "";
    }
  }

  #flushTranscriptPartial(agentId) {
    const entry = this.#subagentTranscripts.get(agentId);
    if (!entry || entry.partial === "") {
      return;
    }
    this.#pushTranscriptLine(entry, entry.partial);
    entry.partial = "";
  }

  // 子 Agent 工具块转成纯文本行，供事后 transcript 使用（不写主终端）。
  #subagentToolLines(name, id, rawArguments) {
    const args = normalizeArguments(rawArguments);
    const lines = [`${this.#sym.top} ${name}${id ? ` (${id})` : ""}`];
    const hasPath =
      args && typeof args === "object" && typeof args.path === "string" && args.path;
    if (hasPath) {
      lines.push(`  ${args.path}`);
    }
    if (
      name === "edit" &&
      args &&
      typeof args === "object" &&
      (args.oldText !== undefined ||
        args.newText !== undefined ||
        Array.isArray(args.edits))
    ) {
      const blocks = Array.isArray(args.edits)
        ? args.edits.filter((block) => block && typeof block === "object")
        : [{ oldText: args.oldText, newText: args.newText }];
      blocks.forEach((block, index) => {
        if (blocks.length > 1) {
          lines.push(`  block ${index + 1}/${blocks.length}`);
        }
        for (const line of String(block.oldText ?? "").split("\n")) {
          if (line !== "") {
            lines.push(`  - ${line}`);
          }
        }
        for (const line of String(block.newText ?? "").split("\n")) {
          if (line !== "") {
            lines.push(`  + ${line}`);
          }
        }
      });
      const max = 24;
      if (lines.length > max) {
        return lines.slice(0, max).concat([`  … (+${lines.length - max} lines)`]);
      }
      return lines;
    }
    let rest = args;
    if (hasPath) {
      rest = { ...args };
      delete rest.path;
    }
    const text = prettyArguments(rest);
    if (text && text !== "{}") {
      lines.push(`  ${clipToWidth(text, MAX_TRANSCRIPT_LINE_CHARS - 2)}`);
    }
    return lines;
  }

  #transcriptPreviewLines(text) {
    const source = String(text ?? "")
      .replace(/\r\n?/g, "\n")
      .replace(/\n+$/, "");
    if (source === "") {
      return [];
    }
    const lines = source.split("\n");
    const shown = lines
      .slice(0, TOOL_PREVIEW_LINES)
      .map((line) => clipToWidth(line, MAX_TRANSCRIPT_LINE_CHARS - 4));
    if (lines.length > TOOL_PREVIEW_LINES) {
      shown.push(`… (+${lines.length - TOOL_PREVIEW_LINES} lines)`);
    }
    return shown.map((line) => `  ${this.#sym.bar} ${line}`);
  }

  // 根 `task` 的工具结果：完整打印（最多 80 行），不再走 8 行 preview。
  #writeTaskResult(text) {
    const source = String(text ?? "")
      .replace(/\r\n?/g, "\n")
      .replace(/\n+$/, "");
    if (source === "") {
      return;
    }
    const lines = source.split("\n");
    const shown = lines.slice(0, MAX_TASK_RESULT_LINES);
    for (const line of shown) {
      if (line === "") {
        this.#write("\n");
      } else {
        this.#writeClipped(`  ${line}`);
      }
    }
    const hidden = lines.length - shown.length;
    if (hidden > 0) {
      this.#writeClipped(`  … (+${hidden} lines)`);
    }
  }

  // 块边界（用户消息、工具调用、回合结束等）：下一段文本重新输出 Agent 标注。
  #endContentBlock() {
    this.#contentAgent = "";
    this.#pendingAgentPrefix = "";
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
    this.#endContentBlock();
    this.#resetSubagentTracking(true);
  }

  startReply() {
    this.#beginOutput();
    this.#flushMdBuffer();
    this.#resetSubagentTracking(true);
    this.#md.reset();
    this.#endContentBlock();
    this.#write("\n");
    this.#ensureAssistantLabel();
    this.#assistantActive = true;
    this.#assistantWroteText = false;
    this.#resumeAssistant = false;
    this.#spinner.start("thinking", "accent");
  }

  content(token, agentId = "") {
    const agent = String(agentId ?? "");
    // 子 Agent 的正文只进事后 transcript，绝不进入主 transcript 的共享缓冲区。
    if (agent) {
      this.#appendTranscriptContent(agent, token);
      const current = this.#subagents.get(agent);
      if (!current || current.state !== "writing") {
        this.#updateSubagent(agent, { state: "writing", tool: "" });
      }
      return;
    }
    if (!this.#assistantActive) {
      this.#beginOutput();
      this.#write("\n");
      this.#ensureAssistantLabel();
      this.#assistantActive = true;
    }
    if (agent !== this.#contentAgent) {
      this.#contentAgent = agent;
      // 根 Agent（空）沿用原有缩进；子 Agent 不再走这条路径。
      this.#pendingAgentPrefix = agent;
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

  toolStart(id, name, rawArguments, agentId = "") {
    const agent = String(agentId ?? "");
    if (agent) {
      this.#flushTranscriptPartial(agent);
      this.#appendTranscriptLines(agent, this.#subagentToolLines(name, id, rawArguments));
      const current = this.#subagents.get(agent);
      this.#updateSubagent(agent, {
        state: "tool",
        tool: name,
        toolCalls: (current?.toolCalls ?? 0) + 1,
      });
      return;
    }
    this.#rootToolNames.set(String(id ?? ""), name);
    this.#rootToolName = name;
    this.#beginOutput();
    this.#flushMdBuffer();
    const t = this.#t;
    this.#write("\n");
    const label = `${t.bold(name)}${id ? ` ${t.dim(`(${id.slice(0, 8)})`)}` : ""}`;
    // 子 Agent 的工具块：`  [explorer#1] ┏ glob` + 内容行再缩进两格。
    const tag = agent ? this.#agentTag(agent) : "";
    const indent = agent ? "    " : "  ";
    this.#writeRow(agent, `  ${tag}${t.fg("borderMuted", this.#sym.top)} ${label}`);
    for (const line of this.#toolCallLines(name, normalizeArguments(rawArguments))) {
      this.#writeRow(agent, `${indent}${t.fg("borderMuted", this.#sym.bar)} ${line}`);
    }
    this.#endContentBlock();
    this.#assistantActive = false;
    this.#assistantWroteText = false;
    this.#resumeAssistant = true;
    this.#currentTool = name;
    this.#spinner.start(`${name} running`, "cyan");
  }

  toolOutput(text, agentId = "") {
    const agent = String(agentId ?? "");
    if (agent) {
      this.#appendTranscriptLines(agent, this.#transcriptPreviewLines(text));
      return;
    }
    this.#beginOutput();
    const indent = agent ? "    " : "  ";
    for (const line of this.#previewLines(text)) {
      this.#writeRow(
        agent,
        `${indent}${this.#t.fg("borderMuted", this.#sym.bar)} ${this.#t.dim(line)}`,
      );
    }
    if (this.#currentTool) {
      this.#spinner.start(`${this.#currentTool} running`, "cyan");
    }
  }

  toolEnd(ok, text, agentId = "", id = "") {
    const agent = String(agentId ?? "");
    if (agent) {
      this.#flushTranscriptPartial(agent);
      if (text) {
        this.#appendTranscriptLines(agent, this.#transcriptPreviewLines(text));
      }
      const outcome = ok ? `${this.#sym.ok} ok` : `${this.#sym.fail} failed`;
      this.#appendTranscriptLines(agent, [`  ${this.#sym.bottom} ${outcome}`]);
      const current = this.#subagents.get(agent);
      this.#updateSubagent(agent, {
        state: "thinking",
        tool: "",
        failed: (current?.failed ?? 0) + (ok ? 0 : 1),
      });
      return;
    }
    this.#beginOutput();
    const t = this.#t;
    const toolName = id ? this.#rootToolNames.get(String(id)) : this.#rootToolName;
    this.#rootToolNames.delete(String(id ?? ""));
    this.#rootToolName = "";
    const colorName = ok ? "success" : "error";
    const body = String(text ?? "");
    if (body) {
      if (toolName === "task") {
        this.#writeTaskResult(body);
      } else {
        for (const line of this.#previewLines(body)) {
          this.#writeRow("", `  ${t.fg("borderMuted", this.#sym.bar)} ${t.dim(line)}`);
        }
      }
    }
    const status = ok
      ? `${t.fg(colorName, this.#sym.ok)} ok`
      : `${t.fg(colorName, this.#sym.fail)} failed`;
    this.#writeRow("", `  ${t.fg(colorName, this.#sym.bottom)} ${status}`);
    this.#resumeAssistant = true;
    this.#currentTool = "";
    this.#spinner.start("thinking", "accent");
  }

  endReply() {
    this.#beginOutput();
    this.#flushMdBuffer();
    this.#resetSubagentTracking(false);
    this.#endContentBlock();
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
    this.#resetSubagentTracking(false);
    this.#endContentBlock();
    const t = this.#t;
    this.#write(`\n${t.fg("warning", `${this.#sym.interrupted} interrupted`)}\n`);
    this.#assistantActive = false;
    this.#assistantWroteText = false;
    this.#resumeAssistant = false;
  }

  approvalRequest(data) {
    this.#beginOutput();
    this.#flushMdBuffer();
    this.#endContentBlock();
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
    this.#endContentBlock();
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

  compaction(data, agentId = "") {
    const agent = String(agentId ?? "");
    const before = data?.tokens_before ?? "?";
    const after = data?.tokens_after ?? "?";
    const reason = data?.forced ? " (overflow)" : "";
    if (agent) {
      this.#flushTranscriptPartial(agent);
      this.#appendTranscriptLines(agent, [
        `${this.#sym.compact} context compacted${reason}: ${before} → ${after} tokens`,
      ]);
      this.#updateSubagent(agent, { state: "compacting", tool: "" });
      return;
    }
    this.#beginOutput();
    this.#flushMdBuffer();
    this.#endContentBlock();
    const t = this.#t;
    this.#write(
      `\n${t.dim(`${this.#sym.compact} context compacted${reason}: ${before} → ${after} tokens`)}\n`,
    );
    this.#assistantActive = false;
    this.#assistantWroteText = false;
    this.#resumeAssistant = true;
    this.#spinner.start("thinking", "accent");
  }

  // 子 Agent 开始：dim 单行，如 `  ↳ explorer#1 started`。
  // 参数既可以是 agent id 字符串，也可以是整帧 data（取其中的 agent_id）。
  // data.background === true 时是后台任务，补一段 `in background (bg-3)`。
  subagentStart(agentOrData) {
    this.#beginOutput();
    this.#flushMdBuffer();
    const named = this.#agentName(agentOrData);
    const agent = named || "subagent";
    const background =
      agentOrData && typeof agentOrData === "object" && agentOrData.background === true;
    if (!background) {
      if (named) {
        this.#updateSubagent(named, {
          state: "starting",
          tool: "",
          toolCalls: 0,
          failed: 0,
        });
      }
      this.#writeClipped(this.#t.dim(`  ${this.#sym.subagent} ${agent} started`));
      return;
    }
    const taskId = String(agentOrData.task_id ?? "").trim();
    const suffix = taskId ? ` in background (${taskId})` : " in background";
    this.#writeClipped(this.#t.dim(`  ${this.#sym.subagent} ${agent} started${suffix}`));
  }

  // 子 Agent 结束：成功 `  ↳ explorer#1 done in 3.9s · 3 tool calls`；
  // 失败 `  ↳ explorer#1 failed: <text>`。
  subagentEnd(data = {}) {
    const agent = this.#agentName(data) || "subagent";
    this.#flushTranscriptPartial(agent);
    this.#subagents.delete(agent);
    this.#syncSubagentStatus();
    this.#beginOutput();
    this.#flushMdBuffer();
    const t = this.#t;
    if (data?.ok === false) {
      const reason = String(data?.text ?? "").trim() || "unknown error";
      this.#writeClipped(t.dim(`  ${this.#sym.subagent} ${agent} failed: ${reason}`));
      return;
    }
    this.#writeClipped(this.#doneLine(agent, data));
  }

  // 后台任务完成（subagent_result 帧，或回合外轮询补打的 pending_results）：
  // 复用前台的生命周期行形态，随后补一段报告摘要（最多 5 行，余下折叠）。
  subagentResult(data = {}) {
    this.#beginOutput();
    this.#flushMdBuffer();
    this.#endContentBlock();
    const t = this.#t;
    const agent = this.#agentName(data) || "subagent";
    if (data?.ok === false) {
      // 失败原因优先取 error，其次取终态（failed/cancelled/timed_out）。
      const reason =
        String(data?.error ?? "").trim() || String(data?.state ?? "").trim() || "unknown error";
      this.#writeClipped(t.dim(`  ${this.#sym.subagent} ${agent} failed: ${reason}`));
    } else {
      this.#writeClipped(this.#doneLine(agent, data));
    }
    this.#writeReportSummary(data?.text);
  }

  // 回合内纠偏消息投递（subagent_steer 帧）：`  ↳ steered explorer#7: 只看 include/ 目录`。
  // 多行文本只取首行；空文本给可读占位。
  subagentSteer(data = {}) {
    this.#beginOutput();
    this.#flushMdBuffer();
    this.#endContentBlock();
    const agent = this.#agentName(data) || "subagent";
    const first = String(data?.text ?? "")
      .split("\n")[0]
      .trim();
    const label = first || "<text 缺失>";
    this.#writeClipped(this.#t.dim(`  ${this.#sym.subagent} steered ${agent}: ${label}`));
  }

  // 根 Agent 的回合内纠偏回执（M5c）：`  ↳ steered: 只看 include/ 目录`。
  // 这是本地命令回显，不是后端帧；走同一条裁剪输出通道。
  steerNotice(text) {
    this.#beginOutput();
    this.#flushMdBuffer();
    this.#endContentBlock();
    const label = String(text ?? "").trim() || "<text 缺失>";
    this.#writeClipped(this.#t.dim(`  ${this.#sym.subagent} steered: ${label}`));
  }

  // 成功行：`  ↳ explorer#1 done in 3.9s · 3 tool calls · 4.4k tokens`。
  #doneLine(agent, data) {
    const t = this.#t;
    const duration = formatDuration(data?.duration_ms);
    const calls = Number(data?.tool_calls);
    const count = Number.isFinite(calls) && calls > 0 ? calls : 0;
    const head = `${agent} done${duration ? ` in ${duration}` : ""}`;
    const parts = [`${count} tool call${count === 1 ? "" : "s"}`];
    // 用量归集（M3）：帧里带 prompt/completion token 时补一段。
    const prompt = Number(data?.prompt_tokens);
    const completion = Number(data?.completion_tokens);
    const total = (Number.isFinite(prompt) ? prompt : 0) + (Number.isFinite(completion) ? completion : 0);
    if (total > 0) {
      parts.push(total < 1000 ? `${total} tokens` : `${(total / 1000).toFixed(1)}k tokens`);
    }
    return t.dim(
      `  ${this.#sym.subagent} ${head} ${this.#sym.dot} ${parts.join(` ${this.#sym.dot} `)}`,
    );
  }

  // 后台任务的报告摘要：缩进 4 格的 dim 行，最多 5 行，超出部分折叠成 `… (+N lines)`。
  // 逐行走 #writeClipped，长行按终端宽度裁剪，不越界。
  #writeReportSummary(text) {
    const source = String(text ?? "")
      .replace(/\r\n?/g, "\n")
      .replace(/\n+$/, "");
    if (source.trim() === "") {
      return;
    }
    const lines = source.split("\n");
    const shown = lines.slice(0, MaxResultLines);
    for (const line of shown) {
      this.#writeClipped(this.#t.dim(`    ${line}`));
    }
    const hidden = lines.length - shown.length;
    if (hidden > 0) {
      this.#writeClipped(this.#t.dim(`    … (+${hidden} lines)`));
    }
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
      if (!rendered.length) {
        this.#write("\n");
        continue;
      }
      // 子 Agent 的文本块：首个非空行加一次 dim 标注，后续行沿用原有缩进。
      const agent = this.#pendingAgentPrefix;
      this.#pendingAgentPrefix = "";
      this.#writeRow(agent, `  ${agent ? this.#agentTag(agent) : ""}${rendered}`);
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
