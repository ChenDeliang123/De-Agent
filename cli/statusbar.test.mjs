import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import { PROMPT_LABEL, formatStatusLine, gitBranch, modeColor, shortCwd } from "./statusbar.mjs";
import { visibleWidth } from "./text.mjs";
import { makeStyler } from "./theme.mjs";

test("shortCwd replaces the home directory with ~", () => {
  assert.equal(shortCwd("/home/u/proj", "/home/u"), "~/proj");
  assert.equal(shortCwd("/home/u", "/home/u"), "~");
  assert.equal(shortCwd("/other", "/home/u"), "/other");
});

test("PROMPT_LABEL stays minimal (no cwd / mode / session on the input line)", () => {
  assert.equal(PROMPT_LABEL, "❯ ");
  assert.ok(!PROMPT_LABEL.includes("/"), "cwd must not sit in front of the cursor");
});

test("gitBranch reads .git/HEAD from an ancestor directory", () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "de-branch-"));
  try {
    fs.mkdirSync(path.join(dir, "sub"), { recursive: true });
    fs.mkdirSync(path.join(dir, ".git"));
    fs.writeFileSync(path.join(dir, ".git", "HEAD"), "ref: refs/heads/feature-x\n");
    assert.equal(gitBranch(path.join(dir, "sub")), "feature-x");

    fs.writeFileSync(path.join(dir, ".git", "HEAD"), "0123456789abcdef\n");
    assert.equal(gitBranch(dir), "0123456");
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test("formatStatusLine composes cwd, branch, mode and session", () => {
  const home = process.env.HOME;
  const line = formatStatusLine(
    {
      cwd: path.join(home, "proj"),
      branch: "main",
      mode: "confirm",
      sessionId: "f920924d-1234-4567",
    },
    80,
  );
  assert.equal(line, "~/proj (main) · confirm · f920924d");
});

test("formatStatusLine drops trailing fields instead of truncating them", () => {
  const line = formatStatusLine(
    {
      cwd: "/very/long/path/to/a/project",
      branch: "main",
      mode: "workspace",
      sessionId: "abcdefgh",
      model: "deepseek-v4-flash",
    },
    24,
  );
  assert.ok(visibleWidth(line) <= 23, `expected <= 23, got ${visibleWidth(line)}: ${line}`);
  assert.ok(!line.includes("deepseek"), "model is the first field to be dropped");
});

test("formatStatusLine degrades to a clipped first field when nothing fits", () => {
  const line = formatStatusLine({ cwd: "/very/long/path/to/a/project", mode: "full" }, 12);
  assert.ok(visibleWidth(line) <= 11, `expected <= 11, got ${line}`);
  assert.ok(line.endsWith("…"));
});

test("formatStatusLine colours the mode by danger level", () => {
  const styler = makeStyler("truecolor");
  const line = formatStatusLine({ cwd: "~", mode: "full" }, 80, styler);
  assert.ok(line.includes(styler.fg("error", "full")), "full access should be red");
  assert.ok(line.includes(styler.fg("dim", "~")));

  assert.equal(modeColor("full"), "error");
  assert.equal(modeColor("confirm"), "warning");
  assert.equal(modeColor("ask"), "warning", "alias ask -> confirm");
  assert.equal(modeColor("workspace"), "muted");
});
