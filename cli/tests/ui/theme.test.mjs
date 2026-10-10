import assert from "node:assert/strict";
import test from "node:test";

import {
  ASCII_SYMBOLS,
  PALETTES,
  SYMBOLS,
  detectColorMode,
  makeStyler,
  pickSymbols,
} from "../../src/ui/theme.mjs";

test("detectColorMode honors NO_COLOR, TTY, TERM and COLORTERM", () => {
  assert.equal(detectColorMode({ NO_COLOR: "1" }, { isTTY: true }), "none");
  assert.equal(detectColorMode({}, { isTTY: false }), "none");
  assert.equal(detectColorMode({}, undefined), "none");
  assert.equal(detectColorMode({ TERM: "dumb" }, { isTTY: true }), "none");
  assert.equal(detectColorMode({ COLORTERM: "truecolor" }, { isTTY: true }), "truecolor");
  assert.equal(detectColorMode({ COLORTERM: "24bit" }, { isTTY: true }), "truecolor");
  assert.equal(detectColorMode({}, { isTTY: true }), "256");
});

test("makeStyler degrades to plain text when mode is none", () => {
  const styler = makeStyler("none");
  assert.equal(styler.fg("accent", "x"), "x");
  assert.equal(styler.bg("accent", "x"), "x");
  assert.equal(styler.bold("x"), "x");
  assert.equal(styler.dim("x"), "x");
});

test("makeStyler emits the right SGR for each mode", () => {
  const truecolor = makeStyler("truecolor");
  assert.ok(truecolor.fg("accent", "x").includes("38;2"));
  assert.ok(truecolor.bg("accent", "x").includes("48;2"));

  const c256 = makeStyler("256");
  assert.ok(c256.fg("accent", "x").includes("38;5"));
  assert.ok(c256.bg("accent", "x").includes("48;5"));

  // 未知色名原样返回，不抛异常。
  assert.equal(truecolor.fg("nope", "x"), "x");
});

test("pickSymbols switches to ASCII via DE_ASCII", () => {
  assert.equal(pickSymbols({}), SYMBOLS);
  assert.equal(pickSymbols({ DE_ASCII: "1" }), ASCII_SYMBOLS);
});

test("makeStyler palette can be switched at runtime", () => {
  const styler = makeStyler("truecolor", PALETTES.dark);
  const dark = styler.fg("accent", "x");
  styler.setPalette(PALETTES.light);
  const light = styler.fg("accent", "x");
  assert.notEqual(dark, light);
  assert.ok(styler.mode === "truecolor");
});
