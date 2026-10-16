import assert from "node:assert/strict";
import test from "node:test";

import { TurnInputAction, TurnInputBuffer } from "../../src/app/turn-input.mjs";

// 可打印字符：keypress 只带 text，key 里没有 ctrl/meta/name。
const typed = (buffer, text) => buffer.handleKey(text, {});
const key = (name, extra = {}) => ({ name, ...extra });

test("turn input buffer accepts printable characters and reports edits", () => {
  const input = new TurnInputBuffer();
  assert.equal(input.isEmpty(), true);
  assert.equal(input.buffer(), "");

  assert.deepEqual(typed(input, "你"), { action: TurnInputAction.Edit });
  assert.deepEqual(typed(input, "好"), { action: TurnInputAction.Edit });
  assert.deepEqual(typed(input, " "), { action: TurnInputAction.Edit });
  assert.deepEqual(typed(input, "world"), { action: TurnInputAction.Edit });
  assert.equal(input.buffer(), "你好 world");
  assert.equal(input.isEmpty(), false);

  // 空 text / 无 key 的控制字符 / 组合键都不进缓冲。
  assert.deepEqual(input.handleKey("", {}), { action: TurnInputAction.None });
  assert.deepEqual(input.handleKey(undefined, {}), { action: TurnInputAction.None });
  assert.deepEqual(input.handleKey("\x03", {}), { action: TurnInputAction.None });
  assert.deepEqual(input.handleKey("\x7f", {}), { action: TurnInputAction.None });
  assert.deepEqual(input.handleKey("a", key("a", { ctrl: true })), { action: TurnInputAction.None });
  assert.deepEqual(input.handleKey("x", key("x", { meta: true })), { action: TurnInputAction.None });
  // 方向键等特殊键（无 text）忽略。
  assert.deepEqual(input.handleKey("", key("left")), { action: TurnInputAction.None });
  assert.equal(input.buffer(), "你好 world");
});

test("turn input buffer backspace removes exactly one code point", () => {
  const input = new TurnInputBuffer();
  for (const char of "a😀中文") {
    typed(input, char);
  }
  assert.equal(input.buffer(), "a😀中文");

  assert.deepEqual(input.handleKey("", key("backspace")), { action: TurnInputAction.Edit });
  assert.equal(input.buffer(), "a😀中");
  assert.deepEqual([...input.buffer()], [..."a😀中文"].slice(0, -1));

  input.handleKey("", key("backspace")); // 删掉「中」
  assert.equal(input.buffer(), "a😀");
  input.handleKey("", key("backspace")); // emoji 是代理对，整体删除，不留半个字符
  assert.equal(input.buffer(), "a");
  assert.equal(Array.from(input.buffer()).length, 1);
  input.handleKey("", key("backspace"));
  assert.equal(input.buffer(), "");
  // 空缓冲上的 Backspace 不产生编辑事件。
  assert.deepEqual(input.handleKey("", key("backspace")), { action: TurnInputAction.None });
});

test("turn input buffer submits on Enter and stays usable", () => {
  const input = new TurnInputBuffer();
  typed(input, "h");
  typed(input, "i");
  assert.deepEqual(input.handleKey("", key("return")), {
    action: TurnInputAction.Submit,
    text: "hi",
  });
  assert.equal(input.buffer(), "");
  assert.equal(input.isEmpty(), true);

  // 提交后可以继续输入；Enter 也接受 name=enter。
  typed(input, "n");
  typed(input, "ext");
  assert.deepEqual(input.handleKey("", key("enter")), {
    action: TurnInputAction.Submit,
    text: "next",
  });

  // 空缓冲 Enter 仍是 submit（空文本），由调用方决定是否发送。
  assert.deepEqual(input.handleKey("", key("return")), {
    action: TurnInputAction.Submit,
    text: "",
  });
});

test("turn input buffer clears on Ctrl+U", () => {
  const input = new TurnInputBuffer();
  typed(input, "abc");
  assert.deepEqual(input.handleKey("u", key("u", { ctrl: true })), {
    action: TurnInputAction.Edit,
  });
  assert.equal(input.buffer(), "");
  // 空缓冲上的 Ctrl+U 不产生编辑事件。
  assert.deepEqual(input.handleKey("u", key("u", { ctrl: true })), {
    action: TurnInputAction.None,
  });
});

test("turn input buffer aborts on Esc and Ctrl+C, discarding the buffer", () => {
  const input = new TurnInputBuffer();
  typed(input, "partial");
  assert.deepEqual(input.handleKey("", key("escape")), { action: TurnInputAction.Abort });
  assert.equal(input.buffer(), "", "回合中断时缓冲作废");

  typed(input, "again");
  assert.deepEqual(input.handleKey("\x03", key("c", { ctrl: true })), {
    action: TurnInputAction.Abort,
  });
  assert.equal(input.buffer(), "");
});

test("turn input buffer caps the buffer by UTF-8 bytes", () => {
  const input = new TurnInputBuffer({ maxBytes: 8 });
  typed(input, "abcdefgh"); // 正好 8 字节
  assert.equal(input.buffer(), "abcdefgh");
  // 放不下的整段输入整体忽略，缓冲保持原样。
  assert.deepEqual(typed(input, "i"), { action: TurnInputAction.None });
  assert.equal(input.buffer(), "abcdefgh");
  assert.deepEqual(typed(input, "wxyz"), { action: TurnInputAction.None });
  // 腾出空间后可以继续输入。
  input.handleKey("", key("backspace"));
  assert.deepEqual(typed(input, "i"), { action: TurnInputAction.Edit });
  assert.equal(input.buffer(), "abcdefgi");

  // 多字节字符按字节计：maxBytes=4 时「中」占 3，字节够才收。
  const wide = new TurnInputBuffer({ maxBytes: 4 });
  typed(wide, "中");
  assert.equal(wide.buffer(), "中");
  assert.deepEqual(typed(wide, "文"), { action: TurnInputAction.None });
  assert.deepEqual(typed(wide, "日本語"), { action: TurnInputAction.None });
  assert.equal(wide.buffer(), "中");

  // 非法/缺省上限回退到默认值。
  const fallback = new TurnInputBuffer({ maxBytes: 0 });
  typed(fallback, "ok");
  assert.equal(fallback.buffer(), "ok");
});
