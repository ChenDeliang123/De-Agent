import assert from "node:assert/strict";
import test from "node:test";

import { renderStartupLogo } from "../../src/ui/logo.mjs";
import { makeStyler, PALETTES } from "../../src/ui/theme.mjs";
import { stripAnsi, visibleWidth } from "../../src/ui/text.mjs";

const TITLES = ["de agent", "v0.1.0"];
const INFO = ["backend=127.0.0.1:18081", "~/proj (main)"];
const HINT = "/help 查看命令 · Esc 中断当前回复 · Ctrl+D 退出";

function render(mode, columns = 80, overrides = {}) {
  const styler = makeStyler(mode, PALETTES.dark);
  return renderStartupLogo(styler, {
    columns,
    titleLines: TITLES,
    info: INFO,
    hint: HINT,
    ...overrides,
  });
}

// 可见宽度（去掉 ANSI），逐行列表。
function visibleLines(text) {
  return text.split("\n").map((line) => ({ line, width: visibleWidth(line) }));
}

function leadingSpaces(line) {
  // 不能用 JS 的 .length 量缩进：CJK 占 2 列，需按列宽累计。
  let lead = 0;
  for (const char of line) {
    if (char !== " ") {
      break;
    }
    lead += 1;
  }
  return lead;
}

function centerOf(line) {
  return leadingSpaces(line) + visibleWidth(line.trimStart()) / 2;
}

test("no-color mode emits no ANSI escape sequences", () => {
  const out = render("none");
  assert.ok(!out.includes("\x1b"), `unexpected escape in: ${JSON.stringify(out)}`);
  assert.ok(out.includes("de agent"));
  assert.ok(out.includes("v0.1.0"));
  assert.ok(out.includes("backend=127.0.0.1:18081"));
  assert.ok(out.includes("/help"));
});

test("truecolor mode applies the gradient to the brand and resets each run", () => {
  const out = render("truecolor");
  const brandLine = stripAnsi(out)
    .split("\n")
    .findIndex((line) => line.trim() === "de agent");
  assert.ok(brandLine > 0, "brand line missing");
  const rawBrand = out.split("\n")[brandLine];
  assert.equal((rawBrand.match(/\[38;2;/g) ?? []).length, "de agent".replace(/ /g, "").length);
  assert.ok(rawBrand.includes("\x1b[0m"), "no reset sequence");
  const colors = [...rawBrand.matchAll(/\[38;2;(\d+);(\d+);(\d+)m/g)].map((m) =>
    m.slice(1).join(","),
  );
  assert.notEqual(colors[0], colors[colors.length - 1], "gradient should vary across characters");
});

test("all visible content stays within the terminal width", () => {
  for (const columns of [100, 80, 60, 45, 30]) {
    for (const mode of ["none", "256", "truecolor"]) {
      const out = render(mode, columns);
      for (const { line, width } of visibleLines(out)) {
        assert.ok(width <= columns, `[${mode}/${columns}] line ${width}: ${JSON.stringify(line)}`);
      }
    }
  }
});

test("each line is centered on its own axis within the terminal width", () => {
  const out = render("none", 80);
  const rows = visibleLines(out).filter(({ line }) => line.trim() !== "");
  assert.ok(rows.length >= 4, "expected title/version/info/hint lines");
  for (const { line } of rows) {
    const center = centerOf(line);
    assert.ok(
      Math.abs(center - 40) <= 1,
      `line not centered: center=${center} ${JSON.stringify(line)}`,
    );
  }
});

test("narrow terminals keep the brand line and first info part only", () => {
  const out = stripAnsi(render("truecolor", 30));
  assert.ok(out.includes("de agent"));
  assert.ok(!out.includes("v0.1.0"), "version line should be dropped when narrow");
  assert.ok(!out.includes("~/proj"), "second info part should be dropped when narrow");
  for (const { width } of visibleLines(render("truecolor", 30))) {
    assert.ok(width <= 30, `line wider than 30 columns: ${width}`);
  }
});

test("empty title lines and blank info parts are skipped", () => {
  const styler = makeStyler("none", PALETTES.dark);
  const out = renderStartupLogo(styler, {
    columns: 80,
    titleLines: [],
    info: ["a", "", "  ", "b"],
    hint: "",
  });
  assert.ok(!out.includes("de agent"));
  assert.ok(out.includes("a · b"));
  assert.ok(!out.includes(" ·  · "));
});

test("light palette with 256 colors produces valid sequences", () => {
  const styler = makeStyler("256", PALETTES.light);
  const out = renderStartupLogo(styler, { columns: 80, titleLines: TITLES, info: INFO, hint: HINT });
  assert.ok(out.includes("[38;5;"), "expected 256-color sequences");
  assert.ok(!out.includes("NaN") && !out.includes("undefined"));
});

test("very narrow terminal still centers without exceeding width", () => {
  const out = render("none", 20);
  for (const { width } of visibleLines(out)) {
    assert.ok(width <= 20, `line wider than 20 columns: ${width}`);
  }
  assert.ok(out.includes("de agent"));
});
