import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

import { loadCliConfig, parseIni, resolveCliConfigPath } from "./config.mjs";

test("parseIni reads sections, keys and values", () => {
  const ini = parseIni(`
; comment
[editor]
external = code --wait
External_Editor = nvim

[other]
x = 1
`);
  assert.deepEqual(ini, {
    editor: { external: "code --wait", external_editor: "nvim" },
    other: { x: "1" },
  });
});

test("resolveCliConfigPath follows explicit > env > cwd > cli dir", () => {
  const base = mkdtempSync(join(tmpdir(), "aiagent-cli-config-"));
  const cwd = join(base, "cwd");
  const cliDir = join(base, "repo", "cli");
  const emptyCwd = join(base, "empty");
  try {
    mkdirSync(join(cwd, "conf"), { recursive: true });
    writeFileSync(join(cwd, "conf", "cli.ini"), "[editor]\n");
    mkdirSync(join(cliDir, "..", "conf"), { recursive: true });
    writeFileSync(join(cliDir, "..", "conf", "cli.ini"), "[editor]\n");
    mkdirSync(emptyCwd, { recursive: true });

    assert.equal(
      resolveCliConfigPath({ explicit: "/tmp/x.ini", env: { AIAGENT_CLI_CONFIG: "/tmp/y.ini" } }),
      "/tmp/x.ini",
    );
    assert.equal(
      resolveCliConfigPath({ env: { AIAGENT_CLI_CONFIG: "/tmp/y.ini" }, cwd, cliDir }),
      "/tmp/y.ini",
    );
    assert.equal(resolveCliConfigPath({ env: {}, cwd, cliDir }), join(cwd, "conf", "cli.ini"));
    assert.equal(
      resolveCliConfigPath({ env: {}, cwd: emptyCwd, cliDir }),
      join(cliDir, "..", "conf", "cli.ini"),
    );
    assert.equal(
      resolveCliConfigPath({ env: {}, cwd: emptyCwd, cliDir: join(base, "nope", "cli") }),
      null,
    );
  } finally {
    rmSync(base, { recursive: true, force: true });
  }
});

test("loadCliConfig reads [editor] external from an explicit file", () => {
  const directory = mkdtempSync(join(tmpdir(), "aiagent-cli-config-"));
  const configPath = join(directory, "cli.ini");
  try {
    writeFileSync(configPath, "[editor]\nexternal = code --wait\n");
    assert.deepEqual(loadCliConfig({ explicit: configPath }), {
      path: configPath,
      externalEditor: "code --wait",
    });
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test("loadCliConfig accepts the external_editor alias and missing file", () => {
  const directory = mkdtempSync(join(tmpdir(), "aiagent-cli-config-"));
  const configPath = join(directory, "cli.ini");
  try {
    writeFileSync(configPath, "[editor]\nexternal_editor = nvim\n");
    assert.equal(loadCliConfig({ explicit: configPath }).externalEditor, "nvim");
    assert.deepEqual(loadCliConfig({ explicit: join(directory, "does-not-exist.ini") }), {
      path: null,
      externalEditor: null,
    });
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});
