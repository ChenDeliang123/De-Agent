import assert from "node:assert/strict";
import test from "node:test";

import { BottomRegion } from "../../src/ui/bottom.mjs";
import { captureOutput } from "../helpers/virtual-terminal.mjs";

test("region keeps the status row at the very bottom while output scrolls", () => {
  const { terminal, out, text } = captureOutput(40);
  const region = new BottomRegion(out);
  region.setFooter("~/proj (main) · confirm");

  region.appendOutput("line one\n");
  region.appendOutput("line two\n");
  region.appendOutput("line three\n");

  const lines = terminal.screen();
  assert.equal(lines.at(-1), "~/proj (main) · confirm", "footer stays at the bottom");
  assert.deepEqual(lines.slice(0, 3), ["line one", "line two", "line three"]);
  assert.ok(!text().includes("line one~/proj"), "footer must not be glued to output");
});

test("region renders the editor block above the status row and positions the cursor", () => {
  const { terminal, out, text } = captureOutput(20);
  const region = new BottomRegion(out);
  region.setFooter("footer");
  const border = "─".repeat(19);
  region.setEditor({ lines: [border, "❯ hi", border], cursorRow: 1, cursorCol: 4 });

  assert.deepEqual(terminal.screen().slice(-4), [border, "❯ hi", border, "footer"]);
  assert.ok(text().endsWith("\x1b[2A\r\x1b[4C"), `cursor misplaced: ${JSON.stringify(text())}`);
});

test("region wraps an unfinished output line instead of letting the terminal wrap", () => {
  const { terminal, out } = captureOutput(10);
  const region = new BottomRegion(out);
  region.setFooter("f");

  // 15 个字符、内容宽 9：折成 2 行，下一次写入必须把两行都擦干净。
  region.appendOutput("abcdefghijklmno");
  assert.deepEqual(terminal.screen().slice(-3), ["abcdefghi", "jklmno", "f"]);

  region.appendOutput("\n");
  assert.deepEqual(terminal.screen().slice(-2), ["abcdefghijklmno", "f"]);
});

test("region suspend erases the block, resume brings the status row back", () => {
  const { terminal, out } = captureOutput(20);
  const region = new BottomRegion(out);
  region.setFooter("footer");
  const border = "─".repeat(19);
  region.setEditor({ lines: [border, "❯ hi", border], cursorRow: 1, cursorCol: 4 });

  region.suspend();
  assert.ok(
    !terminal.lines().some((line) => line.includes("❯ hi")),
    "editor block should be erased while suspended",
  );
  region.appendOutput("question?\n");
  assert.equal(terminal.screen().at(-1), "question?");

  region.resume();
  assert.equal(terminal.screen().at(-1), "footer");
});

test("region works without a TTY by writing through", () => {
  const chunks = [];
  const out = {
    isTTY: false,
    columns: 40,
    write(text) {
      chunks.push(text);
      return true;
    },
  };
  const region = new BottomRegion(out);
  region.setFooter("footer");
  region.appendOutput("plain\n");
  assert.equal(chunks.join(""), "plain\n");
});

test("region clamps the status row to the content width", () => {
  const { terminal, out } = captureOutput(12);
  const region = new BottomRegion(out);
  region.setFooter("a very long footer line");
  assert.equal(terminal.screen().at(-1), "a very long");
});
