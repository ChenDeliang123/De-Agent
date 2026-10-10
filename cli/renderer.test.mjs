import assert from "node:assert/strict";
import test from "node:test";

import { TranscriptRenderer } from "./renderer.mjs";

const SPINNER_RE = /[⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏]/;

function fakeOut(isTTY = true) {
  const chunks = [];
  return {
    isTTY,
    write(text) {
      chunks.push(text);
      return true;
    },
    text: () => chunks.join(""),
  };
}

test("renderer shows a spinner while waiting, clears it on output", () => {
  const out = fakeOut(true);
  const renderer = new TranscriptRenderer(out);

  renderer.startReply();
  assert.equal(renderer.waiting, true, "spinner should be active while waiting");
  assert.ok(out.text().includes("assistant"));
  assert.ok(SPINNER_RE.test(out.text()), "spinner frame should be written");

  renderer.content("Hello");
  assert.equal(renderer.waiting, false, "spinner should clear when content arrives");
  assert.ok(out.text().includes("Hello"));
  assert.ok(out.text().includes("\r\x1b[K"), "waiting line should be cleared");

  renderer.endReply();
  assert.equal(renderer.waiting, false);
});

test("renderer shows a running spinner during a tool call", () => {
  const out = fakeOut(true);
  const renderer = new TranscriptRenderer(out);

  renderer.startReply();
  renderer.toolStart("call-1", "bash", { command: "ls" });
  assert.equal(renderer.waiting, true);
  assert.ok(out.text().includes("bash running"));

  renderer.toolEnd(true, "ok");
  assert.equal(renderer.waiting, true, "waiting for the model after the tool");
  renderer.endReply();
});

test("renderer does not draw a spinner on non-TTY output", () => {
  const out = fakeOut(false);
  const renderer = new TranscriptRenderer(out);

  renderer.startReply();
  assert.equal(renderer.waiting, false);
  renderer.content("Hi");
  renderer.endReply();

  assert.ok(!out.text().includes("\r"), "no carriage returns on non-TTY");
  assert.ok(!SPINNER_RE.test(out.text()), "no spinner frames on non-TTY");
});

test("wait/stopWaiting drive the spinner for long commands", () => {
  const out = fakeOut(true);
  const renderer = new TranscriptRenderer(out);

  renderer.wait("compacting");
  assert.equal(renderer.waiting, true);
  assert.ok(out.text().includes("compacting"));

  renderer.stopWaiting();
  assert.equal(renderer.waiting, false);
});
