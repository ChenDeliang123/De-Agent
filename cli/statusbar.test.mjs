import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import {
  formatPromptLabel,
  formatStatusLine,
  gitBranch,
  shortCwd,
} from "./statusbar.mjs";

test("shortCwd replaces the home directory with ~", () => {
  assert.equal(shortCwd("/home/u/proj", "/home/u"), "~/proj");
  assert.equal(shortCwd("/home/u", "/home/u"), "~");
  assert.equal(shortCwd("/other", "/home/u"), "/other");
});

test("formatPromptLabel composes cwd, branch and mode", () => {
  const home = process.env.HOME;
  const label = formatPromptLabel({
    cwd: path.join(home, "proj"),
    branch: "main",
    mode: "workspace",
  });
  assert.equal(label, "~/proj (main) workspace ❯ ");
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

test("formatStatusLine truncates to the requested width", () => {
  const line = formatStatusLine(
    {
      cwd: "/very/long/path/to/a/project",
      branch: "main",
      mode: "workspace",
      ctxPercent: 12,
      model: "deepseek-v4-flash",
    },
    24,
  );
  assert.ok(line.length <= 24, `expected <=24, got ${line.length}`);
});
