import assert from "node:assert/strict";
import test from "node:test";

import { BottomRegion } from "./bottom.mjs";
import { TranscriptRenderer } from "./renderer.mjs";
import { captureOutput } from "./tests/virtual-terminal.mjs";

const SPINNER_RE = /[⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏]/;

function fakeOut(isTTY = true) {
  const chunks = [];
  return {
    isTTY,
    columns: 80,
    write(text) {
      chunks.push(text);
      return true;
    },
    text: () => chunks.join(""),
  };
}

test("renderer shows a spinner while waiting, clears it on output", () => {
  const out = fakeOut(true);
  const renderer = new TranscriptRenderer(out, { mode: "truecolor" });

  renderer.startReply();
  assert.equal(renderer.waiting, true, "spinner should be active while waiting");
  assert.ok(out.text().includes("de agent"), "assistant label should be printed");
  assert.ok(SPINNER_RE.test(out.text()), "spinner frame should be written");

  renderer.content("Hello\n");
  assert.equal(renderer.waiting, false, "spinner should clear when a line is flushed");
  assert.ok(out.text().includes("Hello"));
  assert.ok(out.text().includes("\r\x1b[K"), "waiting line should be cleared");

  renderer.endReply();
  assert.equal(renderer.waiting, false);
});

test("renderer indents assistant content and draws a user divider", () => {
  const out = fakeOut(true);
  const renderer = new TranscriptRenderer(out, { mode: "none" });
  renderer.user("do it");
  renderer.startReply();
  renderer.content("line1\nline2");
  renderer.endReply();
  const text = out.text();
  assert.ok(text.includes("do it"));
  assert.ok(text.includes("  line1"), "first line should be indented");
  assert.ok(text.includes("  line2"), "continuation line should be indented");
});

test("renderer draws a tool block and a running spinner", () => {
  const out = fakeOut(true);
  const renderer = new TranscriptRenderer(out, { mode: "none" });

  renderer.startReply();
  renderer.toolStart("call-1", "bash", { command: "ls" });
  assert.equal(renderer.waiting, true);
  assert.ok(out.text().includes("bash running"));
  assert.ok(out.text().includes("┏ bash"));

  renderer.toolEnd(true, "file.txt");
  assert.equal(renderer.waiting, true, "waiting for the model after the tool");
  assert.ok(out.text().includes("┗"));
  assert.ok(out.text().includes("file.txt"));
  renderer.endReply();
});

test("renderer does not draw a spinner on non-TTY output", () => {
  const out = fakeOut(false);
  const renderer = new TranscriptRenderer(out, { mode: "none" });

  renderer.startReply();
  assert.equal(renderer.waiting, false);
  renderer.content("Hi");
  renderer.endReply();

  assert.ok(!out.text().includes("\r"), "no carriage returns on non-TTY");
  assert.ok(!SPINNER_RE.test(out.text()), "no spinner frames on non-TTY");
});

test("renderer renders an edit diff from tool arguments", () => {
  const out = fakeOut(true);
  const renderer = new TranscriptRenderer(out, { mode: "none" });
  renderer.startReply();
  renderer.toolStart("c1", "edit", {
    path: "a.mjs",
    oldText: "old line",
    newText: "new line",
  });
  const text = out.text();
  assert.ok(text.includes("a.mjs"));
  assert.ok(text.includes("- old line"));
  assert.ok(text.includes("+ new line"));
  renderer.toolEnd(true, "edited a.mjs");
  renderer.endReply();
});

test("wait/stopWaiting drive the spinner for long commands", () => {
  const out = fakeOut(true);
  const renderer = new TranscriptRenderer(out, { mode: "none" });

  renderer.wait("compacting");
  assert.equal(renderer.waiting, true);
  assert.ok(out.text().includes("compacting"));

  renderer.stopWaiting();
  assert.equal(renderer.waiting, false);
});

test("renderer keeps the status row at the bottom while streaming", () => {
  const { terminal, out } = captureOutput(40);
  const region = new BottomRegion(out);
  region.setFooter("~/proj (main) · confirm");
  const renderer = new TranscriptRenderer(out, { mode: "truecolor", region });

  renderer.user("do it");
  renderer.startReply();
  renderer.content("line1\nline2\n");
  renderer.endReply();

  const lines = terminal.screen();
  assert.equal(lines.at(-1), "~/proj (main) · confirm", "footer stays at the very bottom");
  assert.ok(lines.some((line) => line.includes("line2")), "assistant text is above the footer");
});

test("renderer shows the spinner inside the status row", () => {
  const { terminal, out } = captureOutput(40);
  const region = new BottomRegion(out);
  region.setFooter("~/proj · confirm");
  const renderer = new TranscriptRenderer(out, { mode: "truecolor", region });

  renderer.startReply();
  assert.equal(renderer.waiting, true);
  const waitingRow = terminal.screen().at(-1);
  assert.ok(SPINNER_RE.test(waitingRow), `spinner should live in the status row: ${waitingRow}`);
  assert.ok(waitingRow.includes("~/proj · confirm"), "footer shares the status row");

  renderer.content("done\n");
  assert.equal(renderer.waiting, false);
});

test("renderer without a region keeps writing directly (non-TTY / pipe)", () => {
  const out = fakeOut(false);
  const region = new BottomRegion(out);
  const renderer = new TranscriptRenderer(out, { mode: "none", region });
  renderer.user("hello");
  renderer.endReply();
  const text = out.text();
  assert.ok(text.includes("hello"));
  assert.ok(!text.includes("\x1b[48;"), "no background block without colour support");
});
