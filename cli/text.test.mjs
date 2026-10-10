import assert from "node:assert/strict";
import test from "node:test";

import {
  clipToWidth,
  clipVisible,
  displayWidth,
  stripAnsi,
  visibleWidth,
  wrapVisible,
} from "./text.mjs";

test("visibleWidth ignores ANSI sequences but counts wide characters", () => {
  assert.equal(visibleWidth("\x1b[31mabc\x1b[0m"), 3);
  assert.equal(visibleWidth("\x1b[38;2;1;2;3m你好\x1b[0m"), 4);
  assert.equal(visibleWidth("\x1b]8;;file:///tmp/x\x07link\x1b]8;;\x07"), 4);
  assert.equal(displayWidth("你好"), 4);
});

test("stripAnsi removes CSI and OSC sequences", () => {
  assert.equal(stripAnsi("\x1b[1mhi\x1b[0m"), "hi");
  assert.equal(stripAnsi("a\x1b]8;;u\x07b\x1b]8;;\x07c"), "abc");
});

test("clipVisible keeps escape sequences and marks clipping", () => {
  assert.equal(clipVisible("\x1b[31mabcdef\x1b[0m", 3), "\x1b[31mabc\x1b[0m…");
  assert.equal(clipVisible("abcdef", 3, { ellipsis: false }), "abc");
  assert.equal(clipVisible("abc", 5), "abc");
  assert.equal(clipToWidth("abcdef", 3), "abc…");
});

test("wrapVisible wraps by visible width", () => {
  assert.deepEqual(wrapVisible("abcdef", 3), ["abc", "def"]);
  assert.deepEqual(wrapVisible("你你你", 4), ["你你", "你"]);
  assert.deepEqual(wrapVisible("\x1b[31mabcd\x1b[0m", 2), [
    "\x1b[31mab",
    "cd\x1b[0m",
  ]);
  assert.deepEqual(wrapVisible("", 3), [""]);
});
