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

function visibleLines(text) {
  return text.split("\n").map((line) => ({ line, width: visibleWidth(line) }));
}

// 缩进按列宽累计（CJK / 块字符占位不同，不能用 JS .length）。
function leadingColumns(line) {
  let lead = 0;
  for (const char of line) {
    if (char !== " ") {
      break;
    }
    lead += 1;
  }
  return lead;
}

test("no-color mode emits no ANSI escape sequences", () => {
  const out = render("none");
  assert.ok(!out.includes("\x1b"), `unexpected escape in: ${JSON.stringify(out)}`);
  assert.ok(out.includes("de agent"));
  assert.ok(out.includes("backend=127.0.0.1:18081"));
  assert.ok(out.includes("/help"));
});

test("truecolor mode applies a per-row gradient to the block art", () => {
  const out = render("truecolor");
  const artRows = out.split("\n").filter((line) => line.includes("█"));
  assert.ok(artRows.length >= 4, "expected block-art rows");
  const colors = new Set();
  for (const row of artRows) {
    const match = row.match(/\[38;2;(\d+);(\d+);(\d+)m/);
    assert.ok(match, `row not colored: ${JSON.stringify(row)}`);
    colors.add(match.slice(1).join(","));
    assert.equal((row.match(/\[38;2;/g) ?? []).length, 1, "row should be painted in one color");
  }
  assert.ok(colors.size >= 3, `gradient should vary per row, got ${colors.size}`);
});

test("all visible content stays within the terminal width", () => {
  for (const columns of [120, 80, 64, 50, 30, 20]) {
    for (const mode of ["none", "256", "truecolor"]) {
      const out = render(mode, columns);
      for (const { line, width } of visibleLines(out)) {
        assert.ok(width <= columns, `[${mode}/${columns}] line ${width}: ${JSON.stringify(line)}`);
      }
    }
  }
});

test("block art rows share the same width and indentation", () => {
  const out = render("none", 100);
  const artRows = out.split("\n").filter((line) => line.includes("█"));
  assert.ok(artRows.length >= 4, "expected block-art rows");
  const widths = new Set(artRows.map((line) => visibleWidth(line)));
  assert.equal(widths.size, 1, `art rows must be equal width, got ${[...widths].join(",")}`);
  const leads = new Set(artRows.map((line) => leadingColumns(line)));
  assert.equal(leads.size, 1, `art rows must share indentation, got ${[...leads].join(",")}`);
});

test("every line is centered within the terminal width", () => {
  for (const columns of [100, 80, 64]) {
    const out = render("none", columns);
    for (const { line } of visibleLines(out)) {
      const trimmed = line.trimStart();
      if (trimmed.replace(/ /g, "") === "") {
        continue;
      }
      // 右端可能为空格（块字留白），按「行尾即内容终点」测量：
      // 居中块以等宽网格对齐，首字符列到行尾的距离代表内容区宽度。
      const contentWidth = visibleWidth(trimmed);
      const center = leadingColumns(line) + contentWidth / 2;
      assert.ok(
        Math.abs(center - columns / 2) <= 1,
        `[${columns}] line not centered: center=${center} ${JSON.stringify(line)}`,
      );
    }
  }
});

test("narrow terminals fall back to a text title without block art", () => {
  const out = stripAnsi(render("truecolor", 50));
  assert.ok(!out.includes("█"), "block art must not render when narrow");
  assert.ok(out.includes("de agent v0.1.0"));
  for (const { width } of visibleLines(render("truecolor", 50))) {
    assert.ok(width <= 50, `line wider than 50 columns: ${width}`);
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
  assert.ok(out.includes("a · b"));
  assert.ok(!out.includes(" ·  · "));
  // 标题为空时仍应有块字（品牌名兜底）。
  assert.ok(out.includes("█"));
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
