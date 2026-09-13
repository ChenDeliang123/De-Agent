// 极简 raw-mode 行编辑器（零依赖）。
//
// 目标：在输入过程中提供 slash 指令候选提示、Tab 补全、历史调用，
// 这是 node:readline 的 question() 做不到的。
//
// 拆分：
//   - applyKey(state, str, key, options) 纯函数，便于单测；
//   - LineEditor 负责 TTY 交互与渲染。
//
// 非 TTY（管道）场景不要使用 LineEditor，调用方应退回逐行读取。

import readline from "node:readline";

import { editInExternalEditor, resolveEditorCommand } from "./external-editor.mjs";

export const EDITOR_HOTKEYS = [
  ["Enter", "submit input / run command"],
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

// 粗略的东亚宽字符判定，用于光标横向定位。指令名本身是 ASCII，
// 这里主要保证用户输入中文时输入行不错位。
function charWidth(codePoint) {
  if (codePoint === 0) {
    return 0;
  }
  if (codePoint < 32 || (codePoint >= 0x7f && codePoint < 0xa0)) {
    return 0;
  }
  if (
    (codePoint >= 0x1100 && codePoint <= 0x115f) ||
    codePoint === 0x2329 ||
    codePoint === 0x232a ||
    (codePoint >= 0x2e80 && codePoint <= 0x303e) ||
    (codePoint >= 0x3041 && codePoint <= 0x33ff) ||
    (codePoint >= 0x3400 && codePoint <= 0x4dbf) ||
    (codePoint >= 0x4e00 && codePoint <= 0x9fff) ||
    (codePoint >= 0xa000 && codePoint <= 0xa4cf) ||
    (codePoint >= 0xac00 && codePoint <= 0xd7a3) ||
    (codePoint >= 0xf900 && codePoint <= 0xfaff) ||
    (codePoint >= 0xfe10 && codePoint <= 0xfe19) ||
    (codePoint >= 0xfe30 && codePoint <= 0xfe6f) ||
    (codePoint >= 0xff00 && codePoint <= 0xff60) ||
    (codePoint >= 0xffe0 && codePoint <= 0xffe6) ||
    (codePoint >= 0x1f300 && codePoint <= 0x1faff) ||
    (codePoint >= 0x20000 && codePoint <= 0x3fffd)
  ) {
    return 2;
  }
  return 1;
}

export function displayWidth(text) {
  let width = 0;
  for (const char of String(text)) {
    width += charWidth(char.codePointAt(0));
  }
  return width;
}

export function clipToWidth(text, maxWidth) {
  if (maxWidth <= 0) {
    return "";
  }
  let width = 0;
  let out = "";
  for (const char of String(text)) {
    const w = charWidth(char.codePointAt(0));
    if (width + w > maxWidth) {
      return `${out}…`;
    }
    out += char;
    width += w;
  }
  return out;
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

export function formatCandidate(command, selected, columns = 80) {
  const marker = selected ? "❯" : " ";
  const arg = command.argHint ? ` ${command.argHint}` : "";
  const left = `${marker} /${command.name}${arg}`;
  const description = command.description ? `  ${command.description}` : "";
  return clipToWidth(left + description, Math.max(10, columns - 1));
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
  #drawn = 0;
  #prompt = "";
  #externalEditor;
  #editorCommand;
  #externalEditorActive = false;
  #resolveLine = null;
  #previousRaw = false;

  constructor({
    input,
    output,
    complete,
    history = [],
    maxCandidates = MAX_CANDIDATES,
    externalEditor = editInExternalEditor,
    editorCommand,
  } = {}) {
    this.#input = input ?? process.stdin;
    this.#output = output ?? process.stdout;
    this.#complete = complete;
    this.#maxCandidates = maxCandidates;
    this.#externalEditor = externalEditor;
    this.#editorCommand = editorCommand ?? null;
    this.#state = createEditorState(history);
    if (this.#input.isTTY && !this.#input.__aiagentKeypress) {
      readline.emitKeypressEvents(this.#input);
      this.#input.__aiagentKeypress = true;
    }
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
    this.#drawn = 0;
    this.#redraw();

    this.#previousRaw = previousRaw;

    return new Promise((resolve) => {
      this.#resolveLine = (result) => {
        this.#resolveLine = null;
        input.removeListener("keypress", this.#handleKeypress);
        this.#clearDrawn();
        if (typeof input.setRawMode === "function") {
          input.setRawMode(this.#previousRaw);
        }
        resolve(result);
      };
      input.on("keypress", this.#handleKeypress);
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
    this.#clearDrawn();
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
        input.on("keypress", this.#handleKeypress);
        this.#drawn = 0;
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
    this.#drawn = 0;
    let selected = 0;
    const draw = () => {
      const columns = this.#columns();
      let frame = this.#eraseFrame();
      frame += `\r${clipToWidth(title, columns - 1)}\n`;
      entries.forEach((entry, index) => {
        const marker = index === selected ? "❯" : " ";
        const description = entry.description ? `  ${entry.description}` : "";
        frame += `\r\x1b[K${clipToWidth(`${marker} ${entry.label}${description}`, columns - 1)}`;
        if (index < entries.length - 1) {
          frame += "\n";
        }
      });
      // 光标回到区域首行（标题行），与 #eraseFrame 的“光标在区域顶部”约定一致。
      frame += `\r\x1b[${entries.length}A`;
      this.#drawn = 1 + entries.length;
      this.#output.write(frame);
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
        this.#clearDrawn();
        if (typeof input.setRawMode === "function") {
          input.setRawMode(previousRaw);
        }
        resolve(result);
      };
      input.on("keypress", listener);
    });
  }

  #columns() {
    return this.#output.columns || 80;
  }

  // 清除已绘制的输入/候选区域。约定：调用时光标位于区域首行（顶部），
  // 因此从当前行向下逐行清除，最后把光标还原到首行（不换行）。
  #eraseFrame() {
    if (this.#drawn <= 0) {
      return "";
    }
    let frame = "";
    for (let i = 0; i < this.#drawn; i += 1) {
      frame += "\r\x1b[K";
      if (i < this.#drawn - 1) {
        frame += "\x1b[1B";
      }
    }
    if (this.#drawn > 1) {
      frame += `\x1b[${this.#drawn - 1}A`;
    }
    return frame;
  }

  #clearDrawn() {
    if (this.#drawn <= 0) {
      return;
    }
    this.#output.write(`${this.#eraseFrame()}\r`);
    this.#drawn = 0;
  }

  #redraw() {
    const state = this.#state;
    let frame = this.#eraseFrame();
    frame += `\r${this.#prompt}${state.buffer}`;

    const columns = this.#columns();
    const start = candidateWindow(
      state.candidates.length,
      state.selected,
      this.#maxCandidates,
    );
    const candidates = state.candidates.slice(start, start + this.#maxCandidates);
    for (let i = 0; i < candidates.length; i += 1) {
      const selected = start + i === state.selected;
      frame += `\n\r\x1b[K${formatCandidate(candidates[i], selected, columns)}`;
    }
    if (candidates.length > 0) {
      frame += `\x1b[${candidates.length}A`;
    }
    const column = displayWidth(this.#prompt) + displayWidth(state.buffer.slice(0, state.cursor));
    frame += "\r";
    if (column > 0) {
      frame += `\x1b[${column}C`;
    }
    this.#drawn = 1 + candidates.length;
    this.#output.write(frame);
  }
}
