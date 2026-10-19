import assert from "node:assert/strict";
import test from "node:test";

import { BottomRegion } from "../../src/ui/bottom.mjs";
import { TranscriptRenderer, displayWidth } from "../../src/ui/renderer.mjs";
import { visibleWidth } from "../../src/ui/text.mjs";
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

// 只记录 setSubagentRows 的 region：enabled=false 让主输出仍走 out，
// spinner 也不会调用 region.setWorking。
function fakeRegion() {
  const snapshots = [];
  return {
    enabled: false,
    snapshots,
    setSubagentRows(rows) {
      snapshots.push([...rows]);
    },
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

test("renderer isolates sub-agent content and buffers a transcript", () => {
  const out = fakeOut(true);
  const renderer = new TranscriptRenderer(out, { mode: "none" });

  renderer.startReply();
  renderer.subagentStart("explorer#1");
  renderer.content("Hi", "explorer#1");
  renderer.content(" there\n", "explorer#1");
  renderer.content("next line\n", "explorer#1");
  renderer.toolStart("sub1:c1", "glob", { path: "cli" }, "explorer#1");
  renderer.toolOutput("cli/cli.mjs", "explorer#1");
  renderer.toolEnd(true, "1 file", "explorer#1");
  renderer.content("after the tool\n", "explorer#1");

  const text = out.text();
  assert.ok(text.includes("  ↳ explorer#1 started"), text);
  assert.ok(!text.includes("Hi there"), "sub-agent content must not reach the main transcript");
  assert.ok(!text.includes("next line"));
  assert.ok(!text.includes("[explorer#1]"), "main transcript has no sub-agent detail block");
  assert.ok(!text.includes("sub1:c1"), "sub-agent tool id must not reach the main transcript");

  const transcript = renderer.subagentTranscript("explorer#1");
  assert.ok(transcript, "transcript should exist after the turn");
  assert.ok(transcript.lines.some((line) => line.includes("Hi there")));
  assert.ok(transcript.lines.some((line) => line.includes("next line")));
  assert.ok(transcript.lines.some((line) => line.includes("glob")));
  assert.ok(transcript.lines.some((line) => line.includes("cli/cli.mjs")));
  assert.ok(transcript.lines.some((line) => line.includes("after the tool")));

  renderer.endReply();
  assert.ok(renderer.subagentTranscript("explorer#1"), "endReply keeps the transcript");
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

test("renderer isolates interleaved sub-agent streams", () => {
  const out = fakeOut(true);
  const renderer = new TranscriptRenderer(out, { mode: "none" });

  renderer.startReply();
  renderer.subagentStart("explorer#1");
  renderer.subagentStart("explorer#2");
  renderer.content("A1", "explorer#1");
  renderer.content("B1", "explorer#2");
  renderer.content("A2\n", "explorer#1");
  renderer.content("B2\n", "explorer#2");

  const text = out.text();
  for (const fragment of ["A1", "A2", "B1", "B2"]) {
    assert.ok(!text.includes(fragment), `sub-agent fragment leaked: ${fragment}`);
  }
  assert.deepEqual(renderer.subagentTranscript("explorer#1").lines, ["A1A2"]);
  assert.deepEqual(renderer.subagentTranscript("explorer#2").lines, ["B1B2"]);
  assert.deepEqual(renderer.subagentTranscriptIds(), ["explorer#1", "explorer#2"]);
  renderer.endReply();
});

test("renderer bounds and clears sub-agent transcripts", () => {
  const out = fakeOut(true);
  const renderer = new TranscriptRenderer(out, { mode: "none" });

  renderer.startReply();
  renderer.subagentStart("explorer#1");
  for (let index = 0; index < 250; index += 1) {
    renderer.content(`line ${index}\n`, "explorer#1");
  }
  renderer.content(`${"x".repeat(600)}\n`, "explorer#1");

  const transcript = renderer.subagentTranscript("explorer#1");
  assert.equal(transcript.lines.length, 200);
  assert.equal(transcript.dropped, 51);
  assert.ok(transcript.truncated);
  assert.equal(transcript.lines.at(-1).length, 500);
  assert.ok(transcript.lines.at(-1).endsWith("…"));

  renderer.endReply();
  assert.ok(renderer.subagentTranscript("explorer#1"), "endReply keeps the transcript");
  renderer.user("next turn");
  assert.equal(renderer.subagentTranscript("explorer#1"), null);
  assert.deepEqual(renderer.subagentTranscriptIds(), []);
});

test("renderer prints the full root task result", () => {
  const out = fakeOut(true);
  const renderer = new TranscriptRenderer(out, { mode: "none" });

  renderer.startReply();
  const report = Array.from({ length: 100 }, (_, index) => `report line ${index}`).join("\n");
  renderer.toolStart("call-1", "task", { tasks: [] });
  renderer.toolEnd(true, report, "", "call-1");

  const text = out.text();
  assert.ok(text.includes("  ┏ task (call-1)"), text);
  assert.ok(text.includes("  report line 0"));
  assert.ok(text.includes("  report line 79"));
  assert.ok(!text.includes("  report line 80"), "lines beyond the cap are folded");
  assert.ok(text.includes("  … (+20 lines)"));
  assert.ok(text.includes("  ┗ ✔ ok"));
  renderer.endReply();
});

test("renderer tracks sub-agent status rows", () => {
  const out = fakeOut(true);
  const region = fakeRegion();
  const renderer = new TranscriptRenderer(out, { mode: "none", region });

  renderer.startReply();
  renderer.subagentStart("explorer#1");
  assert.equal(region.snapshots.at(-1).length, 1);
  assert.ok(region.snapshots.at(-1)[0].includes("explorer#1"));
  assert.ok(region.snapshots.at(-1)[0].includes("starting"));

  renderer.toolStart("sub1:c1", "glob", { path: "cli" }, "explorer#1");
  let row = region.snapshots.at(-1)[0];
  assert.ok(row.includes("glob"), row);
  assert.ok(row.includes("1 call"), row);

  renderer.toolEnd(false, "boom", "explorer#1");
  row = region.snapshots.at(-1)[0];
  assert.ok(row.includes("thinking"), row);
  assert.ok(row.includes("1 failed"), row);

  renderer.content("working", "explorer#1");
  row = region.snapshots.at(-1)[0];
  assert.ok(row.includes("writing"), row);

  renderer.subagentEnd({ agent_id: "explorer#1", ok: true, duration_ms: 1000, tool_calls: 1 });
  assert.deepEqual(region.snapshots.at(-1), []);
  renderer.endReply();
});

test("renderer caps sub-agent status rows", () => {
  const out = fakeOut(true);
  const region = fakeRegion();
  const renderer = new TranscriptRenderer(out, { mode: "none", region });

  for (let index = 1; index <= 6; index += 1) {
    renderer.subagentStart(`explorer#${index}`);
  }
  const rows = region.snapshots.at(-1);
  assert.equal(rows.length, 5);
  assert.ok(rows.at(-1).includes("+2 more"), rows.at(-1));
  renderer.endReply();
});

test("renderer does not track background sub-agents in the status area", () => {
  const out = fakeOut(true);
  const region = fakeRegion();
  const renderer = new TranscriptRenderer(out, { mode: "none", region });

  renderer.subagentStart({ agent_id: "explorer#7", task_id: "bg-3", background: true });
  assert.equal(region.snapshots.length, 0, "background start must not create a status row");
  renderer.endReply();
  assert.deepEqual(region.snapshots.at(-1), []);
});

test("renderer paints sub-agent status rows in the bottom region", () => {
  const { terminal, out } = captureOutput(60);
  const region = new BottomRegion(out);
  region.setFooter("footer");
  const renderer = new TranscriptRenderer(out, { mode: "none", region });

  renderer.startReply();
  renderer.subagentStart("explorer#1");
  renderer.toolStart("sub1:c1", "glob", { path: "cli" }, "explorer#1");
  let screen = terminal.screen();
  assert.ok(
    screen.some((line) => line.includes("explorer#1") && line.includes("glob")),
    `status row missing: ${JSON.stringify(screen)}`,
  );
  assert.equal(screen.at(-1), "footer", "footer stays at the very bottom");

  renderer.subagentEnd({ agent_id: "explorer#1", ok: true, duration_ms: 1000, tool_calls: 1 });
  screen = terminal.screen();
  assert.ok(
    !screen.some((line) => line.includes("explorer#1") && line.includes("glob")),
    `status row should disappear on end: ${JSON.stringify(screen)}`,
  );
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

test("renderer clips sub-agent lifecycle lines to the terminal width", () => {
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
  for (const line of out.text().split("\n")) {
    if (line !== "") {
      assert.ok(displayWidth(line) <= 26, `line overflows the terminal: ${JSON.stringify(line)}`);
    }
  }
  assert.ok(out.text().includes("…"), "overlong sub-agent lines are clipped with an ellipsis");
});

test("renderer prints background sub-agent dispatch and result summary", () => {
  const out = fakeOut(true);
  const renderer = new TranscriptRenderer(out, { mode: "none" });

  // 前台子 Agent（无 background 字段）保持原样；后台任务补 `in background (task_id)`。
  renderer.subagentStart("explorer#1");
  renderer.subagentStart({ agent_id: "explorer#7", task_id: "bg-3", background: true });
  // subagent_result 与回合外补打 pending_results 走同一个渲染函数。
  renderer.subagentResult({
    task_id: "bg-3",
    agent_id: "explorer#7",
    agent_type: "explorer",
    state: "done",
    ok: true,
    duration_ms: 12345,
    tool_calls: 4,
    prompt_tokens: 5000,
    completion_tokens: 200,
    cache_hit_tokens: 0,
    text: "l1\nl2\nl3\nl4\nl5\nl6\n",
  });
  const text = out.text();
  assert.ok(text.includes("  ↳ explorer#1 started"), text);
  assert.ok(text.includes("  ↳ explorer#7 started in background (bg-3)"), text);
  assert.ok(text.includes("  ↳ explorer#7 done in 12.3s · 4 tool calls · 5.2k tokens"), text);
  assert.ok(text.includes("    l1"), `missing first report line: ${text}`);
  assert.ok(text.includes("    l5"), `missing fifth report line: ${text}`);
  assert.ok(!text.includes("    l6"), `the sixth report line should be folded: ${text}`);
  // 6 行报告展示 5 行，余下 1 行折叠（末尾换行不算一行）。
  assert.ok(text.includes("    … (+1 lines)"), text);
});

test("renderer renders failed background results with the error reason", () => {
  const out = fakeOut(true);
  const renderer = new TranscriptRenderer(out, { mode: "none" });

  // 无 error 字段时退回终态（timed_out/cancelled/failed）。
  renderer.subagentResult({
    task_id: "bg-4",
    agent_id: "explorer#7",
    ok: false,
    state: "timed_out",
    duration_ms: 60000,
    tool_calls: 2,
    text: "",
  });
  // error 优先，且失败帧的报告正文照常展示。
  renderer.subagentResult({
    task_id: "bg-5",
    agent_id: "general#2",
    ok: false,
    state: "failed",
    error: "boom",
    text: "partial report",
  });
  const text = out.text();
  assert.ok(text.includes("  ↳ explorer#7 failed: timed_out"), text);
  assert.ok(text.includes("  ↳ general#2 failed: boom"), text);
  assert.ok(text.includes("    partial report"), text);
  assert.ok(!text.includes("… (+"), `short reports are not folded: ${text}`);
});

test("renderer clips background result lines to the terminal width", () => {
  const rows = [];
  for (const mode of ["none", "truecolor"]) {
    const out = fakeOut(false);
    out.columns = 40;
    const renderer = new TranscriptRenderer(out, { mode });
    renderer.subagentStart({ agent_id: "explorer#7", task_id: "bg-3", background: true });
    renderer.subagentResult({
      task_id: "bg-3",
      agent_id: "a-very-long-background-agent-name#7",
      ok: true,
      duration_ms: 12345,
      tool_calls: 400,
      prompt_tokens: 500000,
      completion_tokens: 200,
      text: `${"x".repeat(200)}\n${"y".repeat(300)}\nshort`,
    });
    for (const line of out.text().split("\n")) {
      if (line !== "") {
        rows.push({ mode, line });
      }
    }
  }
  for (const { line } of rows) {
    assert.ok(visibleWidth(line) <= 40, `line overflows the terminal: ${JSON.stringify(line)}`);
  }
  assert.ok(
    rows.some(({ line }) => line.includes("…")),
    "overlong background result lines are clipped with an ellipsis",
  );
  // 报告正文没超过 5 行，不出现折叠行。
  assert.ok(!rows.some(({ line }) => line.includes("(+")), "no folding for a 3-line report");
});

test("renderer prints sub-agent steering frames", () => {
  const out = fakeOut(true);
  const renderer = new TranscriptRenderer(out, { mode: "none" });

  renderer.subagentSteer({ task_id: "bg-3", agent_id: "explorer#7", text: "只看 include/ 目录" });
  // 多行文本只取首行。
  renderer.subagentSteer({ agent_id: "general#2", text: "第一行\n第二行" });
  // 空文本给可读占位。
  renderer.subagentSteer({ agent_id: "explorer#1", text: "   " });
  const text = out.text();
  assert.ok(text.includes("  ↳ steered explorer#7: 只看 include/ 目录"), text);
  assert.ok(text.includes("  ↳ steered general#2: 第一行"), text);
  assert.ok(!text.includes("第二行"), `steer frames only show the first line: ${text}`);
  assert.ok(text.includes("  ↳ steered explorer#1: <text 缺失>"), text);
});

test("renderer clips sub-agent steering lines to the terminal width", () => {
  for (const columns of [20, 40, 80]) {
    for (const mode of ["none", "truecolor"]) {
      const out = fakeOut(false);
      out.columns = columns;
      const renderer = new TranscriptRenderer(out, { mode });
      renderer.subagentSteer({
        agent_id: "explorer#7",
        text: "很长的一段纠偏文本 ".repeat(30),
      });
      for (const line of out.text().split("\n")) {
        if (line !== "") {
          assert.ok(
            visibleWidth(line) <= columns,
            `line overflows ${columns} columns: ${JSON.stringify(line)}`,
          );
        }
      }
      assert.ok(out.text().includes("…"), "overlong steering lines are clipped with an ellipsis");
    }
  }
});

test("renderer prints and clips the root-agent steer notice", () => {
  const out = fakeOut(false);
  out.columns = 40;
  const renderer = new TranscriptRenderer(out, { mode: "none" });

  renderer.steerNotice("  ");
  assert.ok(out.text().includes("  ↳ steered: <text 缺失>"), out.text());

  renderer.steerNotice("只看 include/ 目录");
  assert.ok(out.text().includes("  ↳ steered: 只看 include/ 目录"), out.text());

  renderer.steerNotice("长文本 ".repeat(40));
  for (const line of out.text().split("\n")) {
    if (line !== "") {
      assert.ok(
        visibleWidth(line) <= 40,
        `steer notice overflows the terminal: ${JSON.stringify(line)}`,
      );
    }
  }
  assert.ok(out.text().includes("…"), "overlong steer notices are clipped with an ellipsis");
});
