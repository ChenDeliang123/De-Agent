import assert from "node:assert/strict";
import test from "node:test";

import { TranscriptRenderer } from "./renderer.mjs";

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

  renderer.content("Hello");
  assert.equal(renderer.waiting, false, "spinner should clear when content arrives");
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
  assert.ok(text.includes("line1\n  line2"), "continuation lines should be indented");
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

test("wait/stopWaiting drive the spinner for long commands", () => {
  const out = fakeOut(true);
  const renderer = new TranscriptRenderer(out, { mode: "none" });

  renderer.wait("compacting");
  assert.equal(renderer.waiting, true);
  assert.ok(out.text().includes("compacting"));

  renderer.stopWaiting();
  assert.equal(renderer.waiting, false);
});
