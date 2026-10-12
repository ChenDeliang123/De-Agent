import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

import {
  loadCliConfig,
  parseIni,
  resolveCliConfigPath,
  resolveDataDir,
  updateCliUiConfig,
} from "../../src/app/config.mjs";

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

test("resolveDataDir follows DE_HOME > XDG_DATA_HOME/de > ~/.de", () => {
  assert.equal(
    resolveDataDir({ DE_HOME: "/x/de", XDG_DATA_HOME: "/x/xdg", HOME: "/home/u" }),
    "/x/de",
  );
  assert.equal(
    resolveDataDir({ XDG_DATA_HOME: "/x/xdg", HOME: "/home/u" }),
    join("/x/xdg", "de"),
  );
  assert.equal(resolveDataDir({ HOME: "/home/u" }), join("/home/u", ".de"));
  assert.equal(resolveDataDir({}), null);
});

test("resolveCliConfigPath falls back to <data_dir>/conf/cli.ini", () => {
  const base = mkdtempSync(join(tmpdir(), "aiagent-cli-config-"));
  const dataDir = join(base, "data");
  const emptyCwd = join(base, "empty");
  try {
    mkdirSync(join(dataDir, "conf"), { recursive: true });
    writeFileSync(join(dataDir, "conf", "cli.ini"), "[editor]\n");
    mkdirSync(emptyCwd, { recursive: true });
    assert.equal(
      resolveCliConfigPath({
        env: { DE_HOME: dataDir },
        cwd: emptyCwd,
        cliDir: join(base, "nope", "cli"),
      }),
      join(dataDir, "conf", "cli.ini"),
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
      theme: null,
      nerdFont: false,
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
      theme: null,
      nerdFont: false,
    });
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test("loadCliConfig reads [ui] theme and nerd_font", () => {
  const directory = mkdtempSync(join(tmpdir(), "aiagent-cli-config-"));
  const configPath = join(directory, "cli.ini");
  try {
    writeFileSync(configPath, "[ui]\ntheme = light\nnerd_font = true\n");
    const config = loadCliConfig({ explicit: configPath });
    assert.equal(config.theme, "light");
    assert.equal(config.nerdFont, true);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test("updateCliUiConfig creates and updates the [ui] section", () => {
  const directory = mkdtempSync(join(tmpdir(), "aiagent-cli-config-"));
  const configPath = join(directory, "cli.ini");
  try {
    writeFileSync(configPath, "[editor]\nexternal = nvim\n");
    updateCliUiConfig(configPath, { theme: "light" });
    let config = loadCliConfig({ explicit: configPath });
    assert.equal(config.externalEditor, "nvim");
    assert.equal(config.theme, "light");

    updateCliUiConfig(configPath, { theme: "dark" });
    config = loadCliConfig({ explicit: configPath });
    assert.equal(config.theme, "dark");
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});
