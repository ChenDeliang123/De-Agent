import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import test from "node:test";

import {
  LineEditor,
  applyKey,
  createEditorState,
  displayWidth,
  formatCandidate,
  slashCandidates,
} from "./editor.mjs";
import { commands, completeCommands } from "./commands.mjs";

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
