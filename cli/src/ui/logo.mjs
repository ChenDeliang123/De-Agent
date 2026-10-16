// 启动 logo：CLI 首屏的欢迎块（品牌大字 + 元信息 + 键位提示）。
//
// 设计（对齐 pi 的启动头量级）：
//   - 宽终端（>= 64 列）：4 行块字拼出「de」，逐行做品牌渐变
//     （accent → cyan → blue），下方一行品牌名+版本；
//   - 窄终端：退化成一行「de agent vX.Y.Z」文字标题（块字放不下会折行错位）；
//   - 再往下是元信息行（后端地址 · 工作目录）与 dim 键位提示。
//
// 块字符选型：`█`（U+2588）在 Unicode 里属 East Asian Ambiguous，部分中文终端
// 按 2 列渲染；这正是 pi 的 logo 用的字符，主流终端（含本机实测）按 1 列渲染。
// 为保证「即使被按 2 列渲染也不撕裂」，全部按可见宽度居中，且窄屏直接不用块字。
//
// 降级规则（与 theme.mjs 的能力检测一致）：
//   - 颜色 none（非 TTY / NO_COLOR / TERM=dumb）→ 纯文本，无任何 ANSI；
//   - 超宽行按可见宽度裁剪补省略号；
//   - 非 TTY 时调用方不应调用本模块（启动横幅类输出只给交互终端看）。

import { clipToWidth, visibleWidth } from "./text.mjs";

// 品牌像素字：'#'=实心像素。每个像素横向渲染 2 列块字符。
// 为什么用大写「DE」：4×5 像素网格下小写 d 的圆腹与竖干无法同时表达，
// 读起来不是 o 就是 a；大写 D/E 在这个分辨率下字形无歧义。
// D：左竖干 + 右圆腹；E：左竖干 + 三横。
// 两字形均 4 像素宽 × 5 行，行宽由构造保证相等（此前手画块字行宽不一导致撕裂）。
const PIXEL_D = [
  "###.",
  "#..#",
  "#..#",
  "#..#",
  "###.",
];
const PIXEL_E = [
  "###.",
  "#...",
  "##..",
  "#...",
  "###.",
];

// 由像素网格生成块字行（像素 → 2 列块字符，字形间留 2 像素空档）。
// 行尾补齐到等宽：块字按最大宽度整体居中，参差不齐的左缘正是此前撕裂感的来源。
function buildWordmark() {
  const rowToText = (row) => [...row].map((cell) => (cell === "#" ? "██" : "  ")).join("");
  const rows = PIXEL_D.map((row, index) => rowToText(row) + "    " + rowToText(PIXEL_E[index]));
  const maxWidth = Math.max(...rows.map((row) => row.length));
  return rows.map((row) => row.padEnd(maxWidth, " "));
}

const WORDMARK = buildWordmark();

// 块字模式的终端宽度下限（低于此值退化成一行文字标题）。
const WORDMARK_MIN_COLUMNS = 64;

// 品牌渐变：accent → cyan → blue，按位置插值。
function gradientRgb(palette, ratio) {
  const stops = [
    palette.accent ?? [138, 190, 183],
    palette.cyan ?? [0, 215, 255],
    palette.blue ?? [95, 135, 255],
  ];
  const t = Math.max(0, Math.min(1, ratio)) * (stops.length - 1);
  const i = Math.min(Math.floor(t), stops.length - 2);
  const f = t - i;
  const a = stops[i];
  const b = stops[i + 1];
  return [
    Math.round(a[0] + (b[0] - a[0]) * f),
    Math.round(a[1] + (b[1] - a[1]) * f),
    Math.round(a[2] + (b[2] - a[2]) * f),
  ];
}

// 按 styler.mode 生成 RGB 前景上色函数（truecolor / 256 / none）。
function rgbPainter(styler) {
  const mode = styler.mode;
  return (rgb, text) => {
    if (mode === "none" || text === "") {
      return text;
    }
    if (mode === "truecolor") {
      return `\x1b[38;2;${rgb[0]};${rgb[1]};${rgb[2]}m${text}\x1b[0m`;
    }
    const [r, g, b] = rgb;
    let index;
    if (Math.abs(r - g) < 10 && Math.abs(g - b) < 10) {
      index = r < 8 ? 16 : r > 248 ? 231 : 232 + Math.round(((r - 8) / 247) * 24);
    } else {
      index =
        16 +
        36 * Math.round((r / 255) * 5) +
        6 * Math.round((g / 255) * 5) +
        Math.round((b / 255) * 5);
    }
    return `\x1b[38;5;${index}m${text}\x1b[0m`;
  };
}

// 逐字符渐变：把纯文本按可见位置映射到品牌渐变。
function gradientText(text, palette, styler, ratioStart = 0, ratioEnd = 1) {
  const chars = Array.from(String(text));
  const paint = rgbPainter(styler);
  const span = Math.max(1, chars.length - 1);
  return chars
    .map((char, index) => {
      if (char === " ") {
        return char;
      }
      const ratio = ratioStart + ((ratioEnd - ratioStart) * index) / span;
      return paint(gradientRgb(palette, ratio), char);
    })
    .join("");
}

// 生成启动 logo 的完整文本块（首尾各留一个空行），返回字符串。
//
// 参数：
//   styler     共享主题 styler（可运行时换主题；本模块不修改它）
//   columns    终端列数（决定大字/文字标题与裁剪）
//   titleLines 文字标题行（大字模式下只用第一行做品牌名，其余作副标题）
//   info       元信息行内容数组，如 ["backend=...", "~/proj (main)"]，以 · 连接
//   hint       底部快捷键提示（dim），可为空
export function renderStartupLogo(
  styler,
  { columns = 80, titleLines = ["de agent"], info = [], hint = "" } = {},
) {
  const t = styler;
  const width = Math.max(20, Number(columns) || 80);
  const brand = "de agent";
  const bigArt = width >= WORDMARK_MIN_COLUMNS;

  const titleRows = titleLines
    .map((row) => String(row ?? ""))
    .filter((row) => row !== "");
  const parts = info.filter((part) => part && String(part).trim() !== "");
  const bodyRows = [];
  if (parts.length > 0) {
    // 窄屏只保第一条元信息，避免折行。
    bodyRows.push((bigArt ? parts : parts.slice(0, 1)).join(" · "));
  }
  if (hint) {
    bodyRows.push(width < WORDMARK_MIN_COLUMNS && visibleWidth(hint) > width ? "/help" : hint);
  }

  const lines = [""];
  const pushCentered = (plain, style) => {
    const clipped =
      visibleWidth(plain) <= width ? plain : clipToWidth(plain, Math.max(1, width - 1));
    const w = visibleWidth(clipped);
    const pad = w >= width ? "" : " ".repeat(Math.floor((width - w) / 2));
    lines.push(pad + style(clipped));
  };

  if (bigArt) {
    // 大字模式：逐行渐变上色。块字是一个等宽网格 —— 全体按同一缩进居中
    // （左缘对齐），而不是逐行按自身宽度居中（那会让字形参差）。
    const paint = rgbPainter(t);
    const rows = WORDMARK.length;
    const artWidth = Math.max(...WORDMARK.map((line) => visibleWidth(line)));
    const artPad = Math.max(0, Math.floor((width - artWidth) / 2));
    WORDMARK.forEach((line, index) => {
      const rgb = gradientRgb(t.palette ?? {}, index / (rows - 1));
      lines.push(" ".repeat(artPad) + paint(rgb, line));
    });
    // 大字下方一行品牌名 + 版本（标题首行是品牌名，其余行是版本等副标题）。
    const subtitle = [titleRows[0] ?? brand, ...titleRows.slice(1)].join(" ");
    pushCentered(subtitle, (text) => t.dim(text));
  } else {
    // 文字模式：品牌词渐变加粗 + 版本 dim。
    const first = titleRows[0] ?? brand;
    const rest = titleRows.slice(1);
    const styled =
      t.bold(gradientText(first, t.palette ?? {}, t)) +
      (rest.length ? ` ${t.dim(rest.join(" "))}` : "");
    pushCentered(first + (rest.length ? ` ${rest.join(" ")}` : ""), () => styled);
  }

  bodyRows.forEach((row) => pushCentered(row, (text) => t.dim(text)));
  lines.push("");
  return `${lines.join("\n")}\n`;
}
