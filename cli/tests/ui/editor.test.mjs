import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import test from "node:test";

import {
  LineEditor,
  applyKey,
  candidateWindow,
  createEditorState,
  displayWidth,
  formatCandidate,
  slashCandidates,
} from "../../src/ui/editor.mjs";
import { BottomRegion } from "../../src/ui/bottom.mjs";
import { PROMPT_LABEL } from "../../src/ui/statusbar.mjs";
import { commands, completeCommands } from "../../src/app/commands/index.mjs";
import { captureOutput, VirtualTerminal } from "../helpers/virtual-terminal.mjs";

const complete = (prefix) => completeCommands(prefix, commands);

function key(name, extra = {}) {
  return { name, ...extra };
}

class FakeInput extends EventEmitter {
  constructor() {
    super();
    this.isTTY = true;
    this.isRaw = false;
    // 已是 TTY，跳过 emitKeypressEvents，测试里手动 emit。
    this.__aiagentKeypress = true;
  }

  setRawMode(value) {
    this.isRaw = value;
  }

  resume() {}
}

class FakeOutput {
  constructor(columns = 100) {
    this.columns = columns;
    this.chunks = [];
  }

  write(text) {
    this.chunks.push(text);
    return true;
  }

  get text() {
    return this.chunks.join("");
  }
}

test("displayWidth counts wide characters as two columns", () => {
  assert.equal(displayWidth("abc"), 3);
  assert.equal(displayWidth("你好"), 4);
  assert.equal(displayWidth("a你b"), 4);
});

test("formatCandidate marks the selected entry", () => {
  const selected = formatCandidate({ name: "help", argHint: "", description: "Show help" }, true);
  const plain = formatCandidate({ name: "help", argHint: "", description: "Show help" }, false);
  assert.match(selected, /^❯ \/help/);
  assert.match(plain, /^ {2}\/help/);
});

test("slashCandidates only fires for a bare command token", () => {
  assert.deepEqual(slashCandidates("/he", complete).map((c) => c.name), ["help"]);
  assert.deepEqual(slashCandidates("/open s1", complete), []);
  assert.deepEqual(slashCandidates("hello", complete), []);
  assert.deepEqual(
    slashCandidates("/", complete).map((c) => c.name),
    commands.map((c) => c.name),
  );
});

test("applyKey edits, completes and submits", () => {
  const state = createEditorState();

  applyKey(state, "/", undefined, { complete });
  assert.equal(state.buffer, "/");
  assert.equal(state.candidates.length, commands.length);

  applyKey(state, "h", key("h"), { complete });
  applyKey(state, "e", key("e"), { complete });
  assert.deepEqual(state.candidates.map((c) => c.name), ["help"]);

  applyKey(state, "\t", key("tab"), { complete });
  assert.equal(state.buffer, "/help");
  assert.deepEqual(state.candidates, []);

  const action = applyKey(state, "\r", key("return"), { complete });
  assert.deepEqual(action, { type: "submit", text: "/help" });
});

test("applyKey completes on Enter before submitting a partial command", () => {
  const state = createEditorState();
  state.history = [];
  applyKey(state, "/", undefined, { complete });
  applyKey(state, "c", key("c"), { complete });
  applyKey(state, "o", key("o"), { complete });
  assert.deepEqual(state.candidates.map((c) => c.name), ["compact"]);

  const first = applyKey(state, "\r", key("return"), { complete });
  assert.equal(first.type, "redraw");
  assert.equal(state.buffer, "/compact");

  const second = applyKey(state, "\r", key("return"), { complete });
  assert.deepEqual(second, { type: "submit", text: "/compact" });
});

test("applyKey closes candidates on escape and edits with backspace", () => {
  const state = createEditorState();
  applyKey(state, "/", undefined, { complete });
  assert.ok(state.candidates.length > 0);

  applyKey(state, "", key("escape"), { complete });
  assert.deepEqual(state.candidates, []);

  applyKey(state, "a", key("a"), { complete });
  assert.equal(state.buffer, "/a");
  applyKey(state, "", key("backspace"), { complete });
  assert.equal(state.buffer, "/");
});

test("applyKey browses history with arrow keys", () => {
  const state = createEditorState(["first message", "/help"]);
  state.historyIndex = state.history.length;

  applyKey(state, "", key("up"), { complete });
  assert.equal(state.buffer, "/help");
  applyKey(state, "", key("up"), { complete });
  assert.equal(state.buffer, "first message");
  applyKey(state, "", key("down"), { complete });
  assert.equal(state.buffer, "/help");
});

test("applyKey maps Ctrl+G to an external-editor action without mutating state", () => {
  const state = createEditorState();
  state.buffer = "hello";
  state.cursor = 5;

  const action = applyKey(state, "", key("g", { ctrl: true }), { complete });
  assert.deepEqual(action, { type: "external-editor" });
  assert.equal(state.buffer, "hello");
  assert.equal(state.cursor, 5);
});

test("LineEditor resolves readLine on submit and shows candidates", async () => {
  const input = new FakeInput();
  const output = new FakeOutput();
  const editor = new LineEditor({ input, output, complete });

  const pending = editor.readLine("> ");
  input.emit("keypress", "/", undefined);
  assert.ok(output.text.includes("/new"), "candidate list should render /new");
  assert.equal(editor.state.candidates.length, commands.length);

  input.emit("keypress", "h", key("h"));
  input.emit("keypress", "e", key("e"));
  input.emit("keypress", "\t", key("tab"));
  assert.equal(editor.state.buffer, "/help");

  input.emit("keypress", "\r", key("return"));
  const result = await pending;
  assert.equal(result.text, "/help");
  assert.equal(input.isRaw, false, "raw mode should be restored after submit");
  assert.deepEqual(editor.history, ["/help"]);
});

test("LineEditor opens the external editor on Ctrl+G and applies the result", async () => {
  const input = new FakeInput();
  const output = new FakeOutput();
  const calls = [];
  const editor = new LineEditor({
    input,
    output,
    complete,
    editorCommand: "fake-editor",
    externalEditor: async ({ command, content }) => {
      calls.push({ command, content });
      return { status: "complete", content: "edited" };
    },
  });

  const pending = editor.readLine("> ");
  input.emit("keypress", "a", key("a"));
  input.emit("keypress", "b", key("b"));
  input.emit("keypress", "", key("g", { ctrl: true }));
  await new Promise((resolve) => setImmediate(resolve));

  assert.deepEqual(calls, [{ command: "fake-editor", content: "ab" }]);
  assert.equal(editor.state.buffer, "edited");
  assert.equal(editor.state.cursor, "edited".length);
  assert.equal(input.isRaw, true, "raw mode should be restored after editing");

  input.emit("keypress", "\r", key("return"));
  assert.deepEqual(await pending, { text: "edited" });
});

test("LineEditor keeps the current input when the external editor fails", async () => {
  const input = new FakeInput();
  const output = new FakeOutput();
  const editor = new LineEditor({
    input,
    output,
    complete,
    externalEditor: async () => ({ status: "failed" }),
  });

  const pending = editor.readLine("> ");
  input.emit("keypress", "x", key("x"));
  input.emit("keypress", "", key("g", { ctrl: true }));
  await new Promise((resolve) => setImmediate(resolve));

  assert.equal(editor.state.buffer, "x");
  assert.match(output.text, /external editor failed/);

  input.emit("keypress", "\r", key("return"));
  assert.deepEqual(await pending, { text: "x" });
});

test("LineEditor ignores a second Ctrl+G while the editor is open", async () => {
  const input = new FakeInput();
  const output = new FakeOutput();
  let release;
  let calls = 0;
  const gate = new Promise((resolve) => {
    release = resolve;
  });
  const editor = new LineEditor({
    input,
    output,
    complete,
    externalEditor: async () => {
      calls += 1;
      await gate;
      return { status: "complete", content: "done" };
    },
  });

  const pending = editor.readLine("> ");
  input.emit("keypress", "", key("g", { ctrl: true }));
  input.emit("keypress", "", key("g", { ctrl: true }));
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(calls, 1, "editor should only be opened once");

  release();
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(editor.state.buffer, "done");

  input.emit("keypress", "\r", key("return"));
  await pending;
});

test("LineEditor leaves no stale candidate rows after redraw", async () => {
  const input = new FakeInput();
  const output = new FakeOutput(80);
  const terminal = new VirtualTerminal();
  // 先铺一些历史输出，让输入行不在屏幕顶部，从而暴露“向上擦除”的越界。
  terminal.write("previous output\n".repeat(12));
  const rawWrite = output.write.bind(output);
  output.write = (text) => {
    terminal.write(text);
    return rawWrite(text);
  };
  const editor = new LineEditor({ input, output, complete });

  const pending = editor.readLine("[new] ❯ ");
  input.emit("keypress", "/", undefined);
  assert.ok(
    terminal.lines().includes("❯ /new  Start a new session"),
    "first redraw should show the candidate list",
  );

  // 继续输入让候选收窄，旧的候选行必须被清除而不是残留在下方。
  input.emit("keypress", "c", key("c"));
  input.emit("keypress", "o", key("o"));

  const lines = terminal.lines();
  assert.equal(
    lines.filter((line) => line.includes("/new ")).length,
    0,
    "stale /new candidate should be erased",
  );
  assert.equal(
    lines.filter((line) => line.includes("/compact")).length,
    1,
    "/compact should appear exactly once",
  );

  // 第一次 Enter 会把 /co 补全为 /compact，第二次才真正提交。
  input.emit("keypress", "\r", key("return"));
  input.emit("keypress", "\r", key("return"));
  await pending;
});

test("candidateWindow keeps the selection visible while scrolling", () => {
  // 候选不超过窗口时不滚动。
  assert.equal(candidateWindow(5, 4, 8), 0);
  // 选择在窗口内时起点仍为 0。
  assert.equal(candidateWindow(15, 7, 8), 0);
  // 选择越过窗口底部时向下滚动一行。
  assert.equal(candidateWindow(15, 8, 8), 1);
  // 选择到末尾时窗口贴底。
  assert.equal(candidateWindow(15, 14, 8), 7);
});

test("LineEditor scrolls candidates so the selection stays visible", async () => {
  const input = new FakeInput();
  const output = new FakeOutput(80);
  const terminal = new VirtualTerminal();
  const rawWrite = output.write.bind(output);
  output.write = (text) => {
    terminal.write(text);
    return rawWrite(text);
  };
  const editor = new LineEditor({ input, output, complete });

  const pending = editor.readLine("❯ ");
  input.emit("keypress", "/", undefined);

  const selectedLine = () =>
    terminal.lines().find((line) => /^\s*❯ \/[a-z]/.test(line));

  // 连按 8 次下键越过可见窗口，选择应滚到 /status 而不是隐形地停在
  // 未渲染的候选上。
  for (let i = 0; i < 8; i += 1) {
    input.emit("keypress", "", key("down"));
  }
  assert.match(selectedLine(), /❯ \/status/);

  // 继续按到最后一个候选 /quit（候选数量由注册表决定，含 /agents），
  // 再按一次应回到第一个 /new。
  for (let index = 8; index < complete("").length - 1; index += 1) {
    input.emit("keypress", "", key("down"));
  }
  assert.match(selectedLine(), /❯ \/quit/);
  input.emit("keypress", "", key("down"));
  assert.match(selectedLine(), /❯ \/new/);

  input.emit("keypress", "", key("escape"));
  input.emit("keypress", "\r", key("return"));
  await pending;
});

test("LineEditor exits on Ctrl+D with an empty buffer", async () => {
  const input = new FakeInput();
  const output = new FakeOutput();
  const editor = new LineEditor({ input, output, complete });

  const pending = editor.readLine("> ");
  input.emit("keypress", "", key("d", { ctrl: true }));
  const result = await pending;
  assert.equal(result.exit, true);
});

test("LineEditor selectList returns the chosen index and cancels on Esc", async () => {
  const input = new FakeInput();
  const output = new FakeOutput();
  const editor = new LineEditor({ input, output, complete });

  const pending = editor.selectList(["alpha", "beta", "gamma"], "pick one");
  assert.ok(output.text.includes("pick one"));
  assert.ok(output.text.includes("❯ alpha"));

  input.emit("keypress", "", key("down"));
  assert.ok(output.text.includes("❯ beta"));
  input.emit("keypress", "", key("return"));
  assert.equal(await pending, 1);

  const cancel = editor.selectList(["alpha", "beta"], "pick one");
  input.emit("keypress", "", key("escape"));
  assert.equal(await cancel, null);
});

test("applyKey supports Alt+Enter and backslash line continuation", () => {
  const state = createEditorState();
  state.buffer = "hello";
  state.cursor = state.buffer.length;
  const action = applyKey(state, "\r", { name: "return", meta: true });
  assert.equal(action.type, "redraw");
  assert.equal(state.buffer, "hello\n");

  const state2 = createEditorState();
  state2.buffer = "echo hi \\";
  state2.cursor = state2.buffer.length;
  const action2 = applyKey(state2, "\\", key("return"));
  assert.equal(action2.type, "redraw");
  assert.equal(state2.buffer, "echo hi \n");
});

test("LineEditor submits multi-line input", async () => {
  const input = new FakeInput();
  const output = new FakeOutput();
  const editor = new LineEditor({ input, output, complete });

  const pending = editor.readLine("❯ ");
  input.emit("keypress", "a", undefined);
  input.emit("keypress", "\r", { name: "return", meta: true });
  input.emit("keypress", "b", undefined);
  input.emit("keypress", "\r", key("return"));

  const result = await pending;
  assert.equal(result.text, "a\nb");
});

test("LineEditor draws the input box between two borders above the status row", async () => {
  const input = new FakeInput();
  const { terminal, out } = captureOutput(30);
  const region = new BottomRegion(out);
  region.setFooter("~/proj (main) · confirm");
  const editor = new LineEditor({ input, output: out, complete, region });

  const pending = editor.readLine(PROMPT_LABEL);
  input.emit("keypress", "h", key("h"));
  input.emit("keypress", "i", key("i"));

  const border = "─".repeat(29);
  assert.deepEqual(terminal.screen().slice(-4), [
    border,
    "❯ hi",
    border,
    "~/proj (main) · confirm",
  ]);

  input.emit("keypress", "\r", key("return"));
  assert.equal((await pending).text, "hi");
  // 提交后收起输入框，最底部只留状态行。
  assert.deepEqual(terminal.screen().slice(-1), ["~/proj (main) · confirm"]);
  assert.ok(!terminal.screen().some((line) => line.includes("❯ hi")));
});

test("LineEditor wraps long input instead of letting the terminal wrap", async () => {
  const input = new FakeInput();
  const { terminal, out } = captureOutput(20);
  const editor = new LineEditor({ input, output: out, complete });

  const pending = editor.readLine(PROMPT_LABEL);
  for (const ch of "0123456789012345678901234567890123456789") {
    input.emit("keypress", ch, key(ch));
  }

  const lines = terminal.screen();
  assert.ok(
    lines.every((line) => displayWidth(line) <= 19),
    `no row may exceed the content width: ${JSON.stringify(lines)}`,
  );
  assert.ok(lines.includes("❯ 01234567890123456"), "first row holds the prompt + 17 chars");
  assert.ok(lines.includes("6789"), "last row holds the remaining chars");
  assert.equal(
    lines.filter((line) => line === "─".repeat(19)).length,
    2,
    "box has exactly two horizontal borders",
  );

  input.emit("keypress", "\r", key("return"));
  await pending;
});

test("LineEditor reports hidden rows in the border when input scrolls", async () => {
  const input = new FakeInput();
  const { terminal, out } = captureOutput(20);
  const editor = new LineEditor({ input, output: out, complete });

  const pending = editor.readLine(PROMPT_LABEL);
  for (const ch of "x".repeat(160)) {
    input.emit("keypress", ch, key(ch));
  }

  const lines = terminal.screen();
  assert.ok(
    lines.some((line) => line.startsWith("── ↑ ") && line.includes("more")),
    `expected an "↑ n more" hint: ${JSON.stringify(lines)}`,
  );
  assert.ok(
    !lines.some((line) => line.startsWith("── ↓ ")),
    "cursor is at the end, so nothing is hidden below",
  );

  input.emit("keypress", "\r", key("return"));
  await pending;
});
