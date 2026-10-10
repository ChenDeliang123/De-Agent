// 极简 raw-mode 行编辑器（零依赖）。
//
// 目标：在输入过程中提供 slash 指令候选提示、Tab 补全、历史调用，
// 这是 node:readline 的 question() 做不到的。
//
// 拆分：
//   - applyKey(state, str, key, options) 纯函数，便于单测；
//   - layoutInput(text, cursorIndex, width) 纯函数：把输入折成物理行并给出光标位置；
//   - LineEditor 负责按键交互，渲染交给 BottomRegion（输入框 + 底部状态行）。
//     输入框样式对齐 pi：上下两条 `─` 边框，没有多余提示符文字，
//     候选列表排在下边框之下。
//
// 非 TTY（管道）场景不要使用 LineEditor，调用方应退回逐行读取。

import readline from "node:readline";

import { BottomRegion } from "./bottom.mjs";
import { editInExternalEditor, resolveEditorCommand } from "./external-editor.mjs";
import { clipToWidth, displayWidth } from "./text.mjs";
import { detectColorMode, makeStyler } from "./theme.mjs";

// 宽度工具已迁到 text.mjs（ANSI 感知）；这里再导出一次，兼容既有调用点与测试。
export { clipToWidth, displayWidth };

export const EDITOR_HOTKEYS = [
  ["Enter", "submit input / run command"],
  ["Alt / Shift+Enter", "insert a newline (multi-line input)"],
  ["\\ + Enter", "continue on the next line"],
  ["Tab", "complete command"],
  ["Up / Down", "move candidate selection, or browse input history"],
  ["Esc", "close command candidates"],
  ["Ctrl+C", "clear input; press again on empty to exit"],
  ["Ctrl+D", "exit when input is empty"],
  ["Ctrl+G", "edit the current prompt in an external editor"],
  ["Ctrl+A / Ctrl+E", "jump to line start / end"],
  ["Ctrl+U / Ctrl+K", "delete to line start / end"],
  ["Ctrl+W", "delete previous word"],
  ["Left / Right", "move cursor"],
  ["Home / End", "jump to line start / end"],
  ["Backspace / Delete", "delete character"],
];

const MAX_CANDIDATES = 8;

// 输入区最多显示多少物理行（超出的部分滚动，配合边框上的 ↑/↓ 提示）。
const MAX_INPUT_ROWS = 8;

// 把「提示符 + buffer」按可见宽度折成物理行，并给出光标所在的行/列。
// cursorIndex 是 text 的 code unit 下标（调用方负责把提示符长度加上）。
export function layoutInput(text, cursorIndex, width) {
  const limit = Math.max(1, Math.floor(width));
  const rows = [];
  let current = "";
  let currentWidth = 0;
  let cursor = { row: 0, col: 0 };
  let index = 0;
  while (index <= text.length) {
    if (index === cursorIndex) {
      cursor = { row: rows.length, col: currentWidth };
    }
    if (index === text.length) {
      break;
    }
    const char = text[index];
    if (char === "\n") {
      rows.push(current);
      current = "";
      currentWidth = 0;
      index += 1;
      continue;
    }
    const codePoint = text.codePointAt(index);
    const ch = String.fromCodePoint(codePoint);
    const w = displayWidth(ch);
    if (currentWidth + w > limit) {
      rows.push(current);
      current = "";
      currentWidth = 0;
    }
    current += ch;
    currentWidth += w;
    index += ch.length;
  }
  rows.push(current);
  // 光标可能落在最后一行末尾（比如 cursorIndex 在 text 末尾之后）。
  if (cursorIndex >= text.length) {
    cursor = { row: rows.length - 1, col: currentWidth };
  }
  return { rows, cursor };
}

// 输入框上下边框：可带滚动提示（`── ↑ 3 more ─────`）。
export function borderLine(width, hint = "", horizontal = "─") {
  if (!hint) {
    return horizontal.repeat(Math.max(0, width));
  }
  const label = `── ${hint} `;
  const labelWidth = displayWidth(label);
  if (labelWidth >= width) {
    return horizontal.repeat(Math.max(0, width));
  }
  return label + horizontal.repeat(width - labelWidth);
}

// 计算候选列表的滚动窗口起点，保证 selected 始终落在可见窗口内。
// total <= max 时不需要滚动，直接从头显示。
export function candidateWindow(total, selected, max) {
  if (max <= 0 || total <= max) {
    return 0;
  }
  const maxStart = total - max;
  const start = selected - max + 1;
  return Math.max(0, Math.min(start, maxStart));
}

export function formatCandidate(command, selected, columns = 80, styler) {
  const marker = selected ? "❯" : " ";
  const arg = command.argHint ? ` ${command.argHint}` : "";
  const left = `${marker} /${command.name}${arg}`;
  const description = command.description ? `  ${command.description}` : "";
  const plain = left + description;
  if (styler && styler.mode !== "none" && displayWidth(plain) <= columns - 1) {
    const coloredMarker = selected ? styler.fg("accent", marker) : marker;
    const coloredName = styler.bold(`/${command.name}${arg}`);
    const coloredDesc = command.description ? `  ${styler.dim(command.description)}` : "";
    return coloredMarker + coloredName + coloredDesc;
  }
  return clipToWidth(plain, Math.max(10, columns - 1));
}

export function createEditorState(history = []) {
  return {
    buffer: "",
    cursor: 0,
    history,
    historyIndex: history.length,
    historyDraft: "",
    candidates: [],
    selected: 0,
    candidatesSuppressed: false,
  };
}

// 根据当前输入行计算候选列表。只有 "/" 开头且尚未进入参数部分才提示。
export function slashCandidates(buffer, complete) {
  if (typeof complete !== "function" || !buffer.startsWith("/")) {
    return [];
  }
  const token = buffer.slice(1);
  if (token.includes(" ") || token.includes("\t")) {
    return [];
  }
  return complete(token) ?? [];
}

function resetBuffer(state) {
  state.buffer = "";
  state.cursor = 0;
  state.candidates = [];
  state.selected = 0;
  state.candidatesSuppressed = false;
}

function refreshCandidates(state, complete) {
  if (state.candidatesSuppressed) {
    state.candidates = [];
    return;
  }
  state.candidates = slashCandidates(state.buffer, complete);
  if (state.selected >= state.candidates.length) {
    state.selected = 0;
  }
}

function insert(state, text) {
  state.buffer = state.buffer.slice(0, state.cursor) + text + state.buffer.slice(state.cursor);
  state.cursor += text.length;
  state.candidatesSuppressed = false;
}

function deleteWordBackward(state) {
  let index = state.cursor;
  while (index > 0 && /\s/.test(state.buffer[index - 1])) {
    index -= 1;
  }
  while (index > 0 && !/\s/.test(state.buffer[index - 1])) {
    index -= 1;
  }
  state.buffer = state.buffer.slice(0, index) + state.buffer.slice(state.cursor);
  state.cursor = index;
  state.candidatesSuppressed = false;
}

function acceptCompletion(state) {
  const candidate = state.candidates[state.selected];
  if (!candidate) {
    return false;
  }
  const needsArg = Boolean(candidate.argHint && candidate.argHint.length > 0);
  state.buffer = `/${candidate.name}${needsArg ? " " : ""}`;
  state.cursor = state.buffer.length;
  state.candidates = [];
  state.selected = 0;
  state.candidatesSuppressed = false;
  return true;
}

function historyPrevious(state) {
  if (state.history.length === 0) {
    return { type: "none" };
  }
  if (state.historyIndex === state.history.length) {
    state.historyDraft = state.buffer;
  }
  if (state.historyIndex > 0) {
    state.historyIndex -= 1;
  }
  state.buffer = state.history[state.historyIndex] ?? "";
  state.cursor = state.buffer.length;
  state.candidatesSuppressed = false;
  return { type: "redraw" };
}

function historyNext(state) {
  if (state.historyIndex >= state.history.length) {
    return { type: "none" };
  }
  state.historyIndex += 1;
  state.buffer =
    state.historyIndex === state.history.length
      ? state.historyDraft
      : state.history[state.historyIndex];
  state.cursor = state.buffer.length;
  state.candidatesSuppressed = false;
  return { type: "redraw" };
}

// 处理一个按键。返回 { type: "redraw" | "submit" | "exit" | "none", text? }。
export function applyKey(state, str, key, options = {}) {
  const complete = options.complete;
  const k = key ?? {};

  if (k.ctrl) {
    switch (k.name) {
      case "c":
        if (state.buffer) {
          resetBuffer(state);
          return { type: "redraw" };
        }
        return { type: "exit" };
      case "d":
        return state.buffer ? { type: "none" } : { type: "exit" };
      case "g":
        // 交给 LineEditor 异步处理：挂起 raw 模式 -> 外部编辑器 -> 回填。
        return { type: "external-editor" };
      case "a":
        state.cursor = 0;
        return { type: "redraw" };
      case "e":
        state.cursor = state.buffer.length;
        return { type: "redraw" };
      case "u":
        state.buffer = state.buffer.slice(state.cursor);
        state.cursor = 0;
        state.candidatesSuppressed = false;
        refreshCandidates(state, complete);
        return { type: "redraw" };
      case "k":
        state.buffer = state.buffer.slice(0, state.cursor);
        state.candidatesSuppressed = false;
        refreshCandidates(state, complete);
        return { type: "redraw" };
      case "w":
        deleteWordBackward(state);
        refreshCandidates(state, complete);
        return { type: "redraw" };
      default:
        return { type: "none" };
    }
  }

  switch (k.name) {
    case "return":
    case "enter": {
      // 多行输入：Alt/Shift+Enter 或行尾反斜杠续行（不提交）。
      if (k.meta || k.shift) {
        insert(state, "\n");
        state.candidates = [];
        state.candidatesSuppressed = true;
        return { type: "redraw" };
      }
      if (state.cursor === state.buffer.length && state.buffer.endsWith("\\")) {
        state.buffer = `${state.buffer.slice(0, -1)}\n`;
        state.cursor = state.buffer.length;
        state.candidates = [];
        state.candidatesSuppressed = true;
        return { type: "redraw" };
      }
      if (state.candidates.length > 0) {
        const token = state.buffer.startsWith("/")
          ? state.buffer.slice(1).split(/\s+/)[0]
          : state.buffer;
        const exact =
          token.length > 0 &&
          state.candidates.some(
            (candidate) =>
              candidate.name === token || (candidate.aliases ?? []).includes(token),
          );
        if (!exact) {
          acceptCompletion(state);
          return { type: "redraw" };
        }
      }
      return { type: "submit", text: state.buffer };
    }
    case "tab":
      if (acceptCompletion(state)) {
        return { type: "redraw" };
      }
      return { type: "none" };
    case "up":
      if (state.candidates.length > 0) {
        state.selected =
          (state.selected - 1 + state.candidates.length) % state.candidates.length;
        return { type: "redraw" };
      }
      return historyPrevious(state);
    case "down":
      if (state.candidates.length > 0) {
        state.selected = (state.selected + 1) % state.candidates.length;
        return { type: "redraw" };
      }
      return historyNext(state);
    case "escape":
      if (state.candidates.length > 0) {
        state.candidates = [];
        state.selected = 0;
        state.candidatesSuppressed = true;
        return { type: "redraw" };
      }
      return { type: "none" };
    case "backspace":
      if (state.cursor > 0) {
        state.buffer =
          state.buffer.slice(0, state.cursor - 1) + state.buffer.slice(state.cursor);
        state.cursor -= 1;
        state.candidatesSuppressed = false;
        refreshCandidates(state, complete);
        return { type: "redraw" };
      }
      return { type: "none" };
    case "delete":
      if (state.cursor < state.buffer.length) {
        state.buffer =
          state.buffer.slice(0, state.cursor) + state.buffer.slice(state.cursor + 1);
        state.candidatesSuppressed = false;
        refreshCandidates(state, complete);
        return { type: "redraw" };
      }
      return { type: "none" };
    case "left":
      if (state.cursor > 0) {
        state.cursor -= 1;
        return { type: "redraw" };
      }
      return { type: "none" };
    case "right":
      if (state.cursor < state.buffer.length) {
        state.cursor += 1;
        return { type: "redraw" };
      }
      return { type: "none" };
    case "home":
      state.cursor = 0;
      return { type: "redraw" };
    case "end":
      state.cursor = state.buffer.length;
      return { type: "redraw" };
    default:
      break;
  }

  if (str && !k.meta) {
    insert(state, str);
    refreshCandidates(state, complete);
    return { type: "redraw" };
  }
  return { type: "none" };
}

export function renderHotkeys() {
  const lines = ["keys:"];
  for (const [key, description] of EDITOR_HOTKEYS) {
    lines.push(`  ${key.padEnd(20)}${description}`);
  }
  return lines.join("\n");
}

export class LineEditor {
  #input;
  #output;
  #complete;
  #maxCandidates;
  #state;
  #prompt = "";
  #externalEditor;
  #editorCommand;
  #externalEditorActive = false;
  #resolveLine = null;
  #previousRaw = false;
  #styler;
  #region;
  #horizontal;

  constructor({
    input,
    output,
    complete,
    history = [],
    maxCandidates = MAX_CANDIDATES,
    externalEditor = editInExternalEditor,
    editorCommand,
    styler,
    region,
    symbols,
  } = {}) {
    this.#input = input ?? process.stdin;
    this.#output = output ?? process.stdout;
    this.#complete = complete;
    this.#maxCandidates = maxCandidates;
    this.#externalEditor = externalEditor;
    this.#editorCommand = editorCommand ?? null;
    this.#styler = styler ?? makeStyler(detectColorMode(process.env, this.#output));
    this.#horizontal = symbols?.boxHorizontal ?? "─";
    // 共享同一个 BottomRegion（与 renderer 共用，保证底部状态行常驻）；
    // 单测直接构造编辑器时自建一个。
    this.#region =
      region ?? new BottomRegion(this.#output, { enabled: this.#output?.isTTY !== false });
    this.#state = createEditorState(history);
    if (this.#input.isTTY && !this.#input.__aiagentKeypress) {
      readline.emitKeypressEvents(this.#input);
      this.#input.__aiagentKeypress = true;
    }
  }

  get region() {
    return this.#region;
  }

  get state() {
    return this.#state;
  }

  get history() {
    return this.#state.history;
  }

  readLine(prompt = "❯ ") {
    const input = this.#input;
    if (!input.isTTY) {
      return Promise.resolve({ exit: true });
    }
    if (!input.__aiagentKeypress) {
      readline.emitKeypressEvents(input);
      input.__aiagentKeypress = true;
    }
    const previousRaw = input.isRaw === true;
    if (typeof input.setRawMode === "function") {
      input.setRawMode(true);
    }
    input.resume?.();

    resetBuffer(this.#state);
    this.#prompt = prompt;
    this.#previousRaw = previousRaw;

    return new Promise((resolve) => {
      this.#resolveLine = (result) => {
        this.#resolveLine = null;
        input.removeListener("keypress", this.#handleKeypress);
        // 收起输入框（底部状态行保留）。
        this.#region.setEditor(null);
        if (typeof input.setRawMode === "function") {
          input.setRawMode(this.#previousRaw);
        }
        resolve(result);
      };
      // 幂等挂载：先移除再添加，避免重复 readLine 导致一个按键被处理多次。
      input.removeListener("keypress", this.#handleKeypress);
      input.on("keypress", this.#handleKeypress);
      this.#redraw();
    });
  }

  // 稳定的按键处理函数引用，供外部编辑器挂起/恢复时成对 add/removeListener。
  #handleKeypress = (str, key) => {
    if (this.#externalEditorActive) {
      return;
    }
    const action = applyKey(this.#state, str, key, { complete: this.#complete });
    if (action.type === "submit") {
      if (action.text.trim()) {
        this.#state.history.push(action.text);
      }
      this.#state.historyIndex = this.#state.history.length;
      this.#state.historyDraft = "";
      this.#resolveLine?.({ text: action.text });
      return;
    }
    if (action.type === "exit") {
      this.#resolveLine?.({ exit: true });
      return;
    }
    if (action.type === "external-editor") {
      void this.#openExternalEditor();
      return;
    }
    if (action.type === "redraw") {
      this.#redraw();
    }
  };

  // Ctrl+G：挂起当前输入行，交给外部编辑器，退出后回填内容。
  async #openExternalEditor() {
    if (this.#externalEditorActive || typeof this.#externalEditor !== "function") {
      return;
    }
    if (!this.#resolveLine) {
      return;
    }
    const input = this.#input;
    const state = this.#state;
    this.#externalEditorActive = true;

    const command = this.#editorCommand || resolveEditorCommand();
    const content = state.buffer;

    // 挂起：不再抢按键，退出 raw 模式并暂停 stdin，把 TTY 让给编辑器。
    input.removeListener("keypress", this.#handleKeypress);
    // 挂起底部区域：擦除输入框，把终端让给外部编辑器。
    this.#region.suspend();
    if (typeof input.setRawMode === "function") {
      input.setRawMode(false);
    }
    input.pause?.();

    try {
      const result = await this.#externalEditor({ command, content });
      if (result && result.status === "complete") {
        state.buffer = result.content;
        state.cursor = result.content.length;
        state.candidates = [];
        state.selected = 0;
        state.candidatesSuppressed = false;
      } else {
        this.#output.write("\r\x1b[Kexternal editor failed; keeping current input\n");
      }
    } catch (error) {
      this.#output.write(
        `\r\x1b[Kexternal editor failed: ${error.message}; keeping current input\n`,
      );
    } finally {
      // 恢复：重新进入 raw 模式、恢复 stdin、重挂监听并重绘输入行。
      input.resume?.();
      if (typeof input.setRawMode === "function") {
        input.setRawMode(true);
      }
      if (this.#resolveLine) {
        input.removeListener("keypress", this.#handleKeypress);
        input.on("keypress", this.#handleKeypress);
      }
      this.#region.resume();
      if (this.#resolveLine) {
        this.#redraw();
      }
      this.#externalEditorActive = false;
    }
  }

  selectList(items, title = "") {
    const input = this.#input;
    if (!input.isTTY || items.length === 0) {
      return Promise.resolve(null);
    }
    if (!input.__aiagentKeypress) {
      readline.emitKeypressEvents(input);
      input.__aiagentKeypress = true;
    }
    const previousRaw = input.isRaw === true;
    if (typeof input.setRawMode === "function") {
      input.setRawMode(true);
    }
    input.resume?.();

    const entries = items.map((item) =>
      typeof item === "string" ? { label: item, description: "" } : item,
    );
    const width = this.#region.contentWidth;
    let selected = 0;
    const draw = () => {
      const lines = [clipToWidth(String(title), width)];
      entries.forEach((entry, index) => {
        const marker = index === selected ? "❯" : " ";
        const description = entry.description ? `  ${entry.description}` : "";
        const plain = `${marker} ${entry.label}${description}`;
        if (this.#styler.mode !== "none" && displayWidth(plain) <= width) {
          const coloredMarker = index === selected ? this.#styler.fg("accent", marker) : marker;
          const coloredLabel = index === selected ? this.#styler.bold(entry.label) : entry.label;
          const coloredDesc = entry.description
            ? `  ${this.#styler.dim(entry.description)}`
            : "";
          lines.push(`${coloredMarker} ${coloredLabel}${coloredDesc}`);
          return;
        }
        lines.push(clipToWidth(plain, width));
      });
      this.#region.setEditor({ lines, cursorRow: 0, cursorCol: 0 });
    };
    draw();

    return new Promise((resolve) => {
      const listener = (_str, key) => {
        if (!key) {
          return;
        }
        if (key.name === "up") {
          selected = (selected - 1 + entries.length) % entries.length;
          draw();
        } else if (key.name === "down") {
          selected = (selected + 1) % entries.length;
          draw();
        } else if (key.name === "return" || key.name === "enter") {
          finish(selected);
        } else if (
          key.name === "escape" ||
          (key.ctrl && (key.name === "c" || key.name === "d"))
        ) {
          finish(null);
        }
      };
      const finish = (result) => {
        input.removeListener("keypress", listener);
        this.#region.setEditor(null);
        if (typeof input.setRawMode === "function") {
          input.setRawMode(previousRaw);
        }
        resolve(result);
      };
      input.on("keypress", listener);
    });
  }

  // 挂起/恢复底部区域：审批提示、外部编辑器等需要独占终端时使用。
  suspend() {
    this.#region.suspend();
  }

  resume() {
    this.#region.resume();
  }

  // 输入框最多显示多少物理行：终端高度的 30%，夹在 [5, MAX_INPUT_ROWS]。
  #maxInputRows() {
    const rows = Number(this.#output?.rows);
    const height = Number.isFinite(rows) && rows > 0 ? rows : 24;
    return Math.max(5, Math.min(MAX_INPUT_ROWS, Math.floor(height * 0.3)));
  }

  // 组装输入框块：上边框 + 可见输入行 + 下边框 + 候选列表（在下边框之下，对齐 pi）。
  #layout() {
    const state = this.#state;
    const width = this.#region.contentWidth;
    const text = this.#prompt + state.buffer;
    const { rows, cursor } = layoutInput(text, this.#prompt.length + state.cursor, width);
    const maxRows = this.#maxInputRows();
    const start =
      rows.length > maxRows
        ? Math.min(Math.max(0, cursor.row - maxRows + 1), rows.length - maxRows)
        : 0;
    const visible = rows.slice(start, start + maxRows);
    const hiddenAbove = start;
    const hiddenBelow = rows.length - (start + visible.length);

    const lines = [
      borderLine(width, hiddenAbove > 0 ? `↑ ${hiddenAbove} more` : "", this.#horizontal),
    ];
    for (const row of visible) {
      lines.push(row);
    }
    lines.push(
      borderLine(width, hiddenBelow > 0 ? `↓ ${hiddenBelow} more` : "", this.#horizontal),
    );

    const windowStart = candidateWindow(
      state.candidates.length,
      state.selected,
      this.#maxCandidates,
    );
    const candidates = state.candidates.slice(
      windowStart,
      windowStart + this.#maxCandidates,
    );
    for (let i = 0; i < candidates.length; i += 1) {
      const selected = windowStart + i === state.selected;
      lines.push(formatCandidate(candidates[i], selected, width + 1, this.#styler));
    }

    return {
      lines,
      cursorRow: 1 + Math.max(0, cursor.row - start),
      cursorCol: cursor.col,
    };
  }

  #redraw() {
    if (!this.#resolveLine) {
      return;
    }
    this.#region.setEditor(this.#layout());
  }
}
