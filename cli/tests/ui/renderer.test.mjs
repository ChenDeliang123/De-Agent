import assert from "node:assert/strict";
import test from "node:test";

import { BottomRegion } from "../../src/ui/bottom.mjs";
import { TranscriptRenderer, displayWidth } from "../../src/ui/renderer.mjs";
import { captureOutput } from "../helpers/virtual-terminal.mjs";

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

test("renderer renders every block of a multi-block edit", () => {
  const out = fakeOut(true);
  const renderer = new TranscriptRenderer(out, { mode: "none" });
  renderer.startReply();
  renderer.toolStart("c2", "edit", {
    path: "a.mjs",
    edits: [
      { oldText: "old one", newText: "new one" },
      { oldText: "old two", newText: "new two" },
    ],
  });
  const text = out.text();
  assert.ok(text.includes("block 1/2"), "multi-block edit should label each block");
  assert.ok(text.includes("block 2/2"));
  assert.ok(text.includes("- old one") && text.includes("+ new one"));
  assert.ok(text.includes("- old two") && text.includes("+ new two"));
  renderer.toolEnd(true, "edited a.mjs: 2 replacement(s)");
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

test("region renderer paints the submitted input as a filled block", () => {
  const { terminal, out, text } = captureOutput(40);
  const region = new BottomRegion(out);
  region.setFooter("footer");
  const renderer = new TranscriptRenderer(out, { mode: "truecolor", region });

  renderer.user("do it");

  // userBg = #343541（dark 主题）：整行底色 + 左右各 1 列内边距。
  assert.ok(text().includes("\x1b[48;2;52;53;65m"), "user message should be painted with userBg");
  const rows = terminal.lines();
  assert.ok(
    rows.some((row) => row.startsWith("  do it")),
    `user text keeps a 2-space gutter: ${JSON.stringify(rows)}`,
  );
  assert.ok(rows.some((row) => row.includes("footer")), "footer stays below the block");
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

test("renderer labels a sub-agent text block once, at its first line", () => {
  const out = fakeOut(true);
  const renderer = new TranscriptRenderer(out, { mode: "none" });

  renderer.startReply();
  renderer.content("Hi", "explorer#1");
  renderer.content(" there\n", "explorer#1");
  renderer.content("next line\n", "explorer#1");
  const text = out.text();
  assert.ok(
    text.includes("  [explorer#1] Hi there"),
    `first line of the block should carry the agent label: ${JSON.stringify(text)}`,
  );
  assert.ok(text.includes("  next line"), "continuation lines keep the plain indent");
  assert.equal(
    (text.match(/\[explorer#1\]/g) ?? []).length,
    1,
    "consecutive tokens from the same agent share one label",
  );

  // 工具调用打断文本块：同一 Agent 的下一段文本重新标注。
  renderer.toolStart("sub1:c1", "glob", { path: "cli" }, "explorer#1");
  renderer.toolEnd(true, "", "explorer#1");
  renderer.content("after the tool\n", "explorer#1");
  const second = out.text();
  assert.ok(second.includes("  [explorer#1] after the tool"), "a new block is labelled again");
  assert.equal((second.match(/\[explorer#1\]/g) ?? []).length, 4, "block + tool + tool + block");
  renderer.endReply();
});

test("renderer keeps root-agent content and tools unchanged", () => {
  const out = fakeOut(true);
  const renderer = new TranscriptRenderer(out, { mode: "none" });

  renderer.startReply();
  renderer.content("plain\n");
  renderer.toolStart("call-1", "bash", { command: "ls" });
  renderer.toolOutput("file.txt");
  renderer.toolEnd(true, "done");
  const text = out.text();
  assert.ok(text.includes("  plain"), "root content keeps the 2-space indent");
  assert.ok(text.includes("  ┏ bash (call-1)"), "root tool header is unchanged");
  assert.ok(text.includes("  ┃ file.txt"), "root tool output is unchanged");
  assert.ok(text.includes("  ┗ ✔ ok"), "root tool footer is unchanged");
  assert.ok(!text.includes("[explorer"), "root frames carry no agent label");
  assert.ok(!/^ {2}\[/m.test(text), "no bracketed label on root lines");
  renderer.endReply();
});

test("renderer marks sub-agent tool blocks with a label and extra indent", () => {
  const out = fakeOut(true);
  const renderer = new TranscriptRenderer(out, { mode: "none" });

  renderer.startReply();
  renderer.toolStart("sub1:c1", "glob", { path: "cli" }, "explorer#1");
  renderer.toolOutput("cli/cli.mjs", "explorer#1");
  renderer.toolEnd(true, "1 file", "explorer#1");
  const text = out.text();
  assert.ok(
    text.includes("  [explorer#1] ┏ glob (sub1:c1)"),
    `sub-agent header carries the label: ${JSON.stringify(text)}`,
  );
  assert.ok(text.includes("    ┃ cli"), "sub-agent arguments are indented two more spaces");
  assert.ok(text.includes("    ┃ cli/cli.mjs"), "sub-agent output is indented two more spaces");
  assert.ok(text.includes("    ┃ 1 file"), "sub-agent end payload keeps the indent");
  assert.ok(text.includes("  [explorer#1] ┗ ✔ ok"), "sub-agent footer keeps the label");
  renderer.endReply();
});

test("renderer prints sub-agent lifecycle lines", () => {
  const out = fakeOut(true);
  const renderer = new TranscriptRenderer(out, { mode: "none" });

  renderer.subagentStart("explorer#1");
  // 整帧 data 与裸 agent id 都接受。
  renderer.subagentStart({ agent_id: "general#2", parent_tool_call_id: "c9" });
  renderer.subagentEnd({
    agent_id: "explorer#1",
    ok: true,
    duration_ms: 3900,
    tool_calls: 3,
    text: "",
  });
  renderer.subagentEnd({
    agent_id: "explorer#1",
    ok: true,
    duration_ms: 64000,
    tool_calls: 1,
    text: "",
  });
  renderer.subagentEnd({
    agent_id: "explorer#1",
    ok: false,
    duration_ms: 1200,
    tool_calls: 1,
    text: "timed out",
  });
  // 用量归集（M3）：帧里带 token 时在生命周期行补一段；不带则保持原样。
  renderer.subagentEnd({
    agent_id: "explorer#2",
    ok: true,
    duration_ms: 24000,
    tool_calls: 15,
    prompt_tokens: 4000,
    completion_tokens: 400,
    cache_hit_tokens: 1000,
  });
  renderer.subagentEnd({
    agent_id: "explorer#3",
    ok: true,
    duration_ms: 2000,
    tool_calls: 2,
    prompt_tokens: 400,
    completion_tokens: 40,
  });
  const text = out.text();
  assert.ok(text.includes("  ↳ explorer#1 started"), text);
  assert.ok(text.includes("  ↳ explorer#2 done in 24.0s · 15 tool calls · 4.4k tokens"), text);
  assert.ok(text.includes("  ↳ explorer#3 done in 2.0s · 2 tool calls · 440 tokens"), text);
  assert.ok(text.includes("  ↳ general#2 started"), text);
  assert.ok(text.includes("  ↳ explorer#1 done in 3.9s · 3 tool calls"), text);
  assert.ok(text.includes("  ↳ explorer#1 done in 1m 4s · 1 tool call"), text);
  assert.ok(text.includes("  ↳ explorer#1 failed: timed out"), text);
});

test("renderer clips sub-agent lines to the terminal width", () => {
  const out = fakeOut(false);
  out.columns = 26;
  const renderer = new TranscriptRenderer(out, { mode: "none" });

  renderer.subagentStart("explorer#1");
  renderer.subagentEnd({
    agent_id: "explorer#1",
    ok: false,
    duration_ms: 1200,
    tool_calls: 1,
    text: "boom ".repeat(20),
  });
  renderer.toolStart("sub1:c1", "bash", { command: "x".repeat(120) }, "explorer#1");
  renderer.toolOutput("y".repeat(120), "explorer#1");
  renderer.toolEnd(true, "z".repeat(120), "explorer#1");
  for (const line of out.text().split("\n")) {
    if (line !== "") {
      assert.ok(displayWidth(line) <= 26, `line overflows the terminal: ${JSON.stringify(line)}`);
    }
  }
  assert.ok(out.text().includes("…"), "overlong sub-agent lines are clipped with an ellipsis");
});
