import assert from "node:assert/strict";
import { existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

import {
  editInExternalEditor,
  resolveEditorCommand,
  stripBom,
} from "./external-editor.mjs";

const fixturePath = fileURLToPath(new URL("./tests/fixtures/fake-external-editor.mjs", import.meta.url));

async function runExternalEditor(flag) {
  const captureDirectory = mkdtempSync(join(tmpdir(), "aiagent-external-editor-test-"));
  const capturePath = join(captureDirectory, "capture.json");
  try {
    const command = `${process.execPath} ${fixturePath} ${capturePath}${flag ? ` ${flag}` : ""}`;
    const result = await editInExternalEditor({ command, content: "original" });
    const capture = JSON.parse(readFileSync(capturePath, "utf-8"));
    return { result, capture };
  } finally {
    rmSync(captureDirectory, { recursive: true, force: true });
  }
}

test("stripBom removes a leading UTF-8 BOM", () => {
  assert.equal(stripBom("\uFEFFhello"), "hello");
  assert.equal(stripBom("hello"), "hello");
});

test("resolveEditorCommand follows flag > config > env > default", () => {
  assert.equal(
    resolveEditorCommand({ flag: "code --wait", configured: "nvim", env: { AIAGENT_EDITOR: "emacs" } }),
    "code --wait",
  );
  assert.equal(
    resolveEditorCommand({ configured: "nvim", env: { AIAGENT_EDITOR: "emacs" } }),
    "nvim",
  );
  assert.equal(
    resolveEditorCommand({ env: { AIAGENT_EDITOR: "nvim", VISUAL: "vim", EDITOR: "nano" } }),
    "nvim",
  );
  assert.equal(resolveEditorCommand({ env: { VISUAL: "vim", EDITOR: "nano" } }), "vim");
  assert.equal(resolveEditorCommand({ env: { EDITOR: "nano" } }), "nano");
  assert.equal(resolveEditorCommand({ env: {}, platform: "linux" }), "nano");
  assert.equal(resolveEditorCommand({ env: {}, platform: "win32" }), "notepad");
});

test("editInExternalEditor edits prompt.md in a private temp directory", async () => {
  const { result, capture } = await runExternalEditor();
  const directory = dirname(capture.filePath);

  assert.deepEqual(result, { status: "complete", content: "edited" });
  assert.equal(dirname(directory), tmpdir());
  assert.match(basename(directory), /^aiagent-editor-.+$/);
  assert.equal(basename(capture.filePath), "prompt.md");
  assert.deepEqual(capture.entries, ["prompt.md"]);
  assert.equal(capture.content, "original");
  if (process.platform !== "win32") {
    assert.equal(capture.directoryMode & 0o077, 0, "temp directory should be private");
  }
  assert.equal(existsSync(directory), false, "temp directory should be cleaned up");
});

test("editInExternalEditor keeps the original content when the editor fails", async () => {
  const { result, capture } = await runExternalEditor("--fail");
  assert.deepEqual(result, { status: "failed" });
  assert.equal(existsSync(dirname(capture.filePath)), false);
});

test("editInExternalEditor returns empty content when the editor clears the prompt", async () => {
  const { result } = await runExternalEditor("--empty");
  assert.deepEqual(result, { status: "complete", content: "" });
});

test("editInExternalEditor reports failure when the editor cannot start", async () => {
  const result = await editInExternalEditor({
    command: "definitely-not-a-real-editor-xyz",
    content: "original",
  });
  assert.deepEqual(result, { status: "failed" });
});
