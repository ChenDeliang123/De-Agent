import assert from "node:assert/strict";
import test from "node:test";

import { createMarkdownRenderer } from "./markdown.mjs";
import { makeStyler } from "./theme.mjs";

function plainMarkdown() {
  return createMarkdownRenderer(makeStyler("none"));
}

test("markdown renders headings, lists, quotes and rules in plain mode", () => {
  const md = plainMarkdown();
  assert.deepEqual(md.renderLine("# Title"), ["Title"]);
  assert.deepEqual(md.renderLine("- item"), ["• item"]);
  assert.deepEqual(md.renderLine("1. first"), ["1. first"]);
  assert.deepEqual(md.renderLine("> quote"), ["│ quote"]);
  assert.equal(md.renderLine("---")[0].length, 40);
});

test("markdown keeps code block state across lines", () => {
  const md = plainMarkdown();
  assert.deepEqual(md.renderLine("```js"), ["╭─ js"]);
  assert.deepEqual(md.renderLine("const x = 1;"), ["│ const x = 1;"]);
  assert.deepEqual(md.renderLine("```"), ["╰─"]);
  // 退出代码块后恢复正常解析
  assert.deepEqual(md.renderLine("- a"), ["• a"]);
});

test("markdown strips inline code markers", () => {
  const md = plainMarkdown();
  assert.deepEqual(md.renderLine("use `foo()` now"), ["use foo() now"]);
});

test("markdown reset() leaves code block state", () => {
  const md = plainMarkdown();
  md.renderLine("```");
  md.reset();
  assert.deepEqual(md.renderLine("# h"), ["h"]);
});
