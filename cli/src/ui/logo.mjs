// 启动 logo：把「de agent」的启动界面从一行纯文本升级为带品牌感的欢迎块。
//
// 设计（借鉴 pi 的克制风格：一行品牌标题 + 元信息 + 键位提示）：
//   - 标题「de agent vX.Y.Z」逐字符做品牌渐变（accent → cyan → blue）；
//     「agent」与版本号保持 dim，突出品牌词但不喧哗；
//   - 下一行元信息：后端地址 · 工作目录；
//   - 再一行 dim 快捷键提示。
//
// 为什么不用块字符拼字（█▄▀）：这些码位在 Unicode 里是 East Asian Ambiguous，
// 中文终端常按 2 列渲染，块画会当场错位撕裂；纯文本 + 渐变在任何字体/宽度设置
// 下都稳定。全部内容用可见宽度（含 ANSI 感知）计算居中，CJK 也对齐。
//
// 降级规则（与 theme.mjs 的能力检测一致）：
//   - 颜色 none（非 TTY / NO_COLOR / TERM=dumb）→ 纯文本，无任何 ANSI；
//   - 窄终端 → 只保留标题与短信息，超宽行按可见宽度裁剪补省略号；
//   - 非 TTY 时调用方不应调用本模块（启动横幅类输出只给交互终端看）。

import { clipToWidth, visibleWidth } from "./text.mjs";

// 品牌渐变：accent → cyan → blue，按字符位置插值。
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
  const colorize = (rgb, text) => {
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
  return colorize;
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
// 标题块由 titleLines 逐行给出：首行是品牌词（渐变加粗），其余行保持 dim
// （用于版本号等），窄终端自动退化成单行。每行都保证不超过终端宽度。
//
// 参数：
//   styler     共享主题 styler（可运行时换主题；本模块不修改它）
//   columns    终端列数（用于居中与裁剪）
//   titleLines 标题行数组，如 ["de agent", "v0.1.0"]；空数组则不打印标题
//   info       元信息行内容数组，如 ["backend=...", "~/proj (main)"]，以 · 连接
//   hint       底部快捷键提示（dim），可为空
export function renderStartupLogo(
  styler,
  { columns = 80, titleLines = ["de agent"], info = [], hint = "" } = {},
) {
  const t = styler;
  const width = Math.max(20, Number(columns) || 80);
  const brand = "de agent";
  const narrow = width < 44;

  // ---- 第一遍：整理内容（只算宽度，不裁剪） --------------------------------
  const titleRows = (narrow ? titleLines.slice(0, 1) : titleLines)
    .map((row) => String(row ?? ""))
    .filter((row) => row !== "");
  const bodyRows = [];
  const parts = info.filter((part) => part && String(part).trim() !== "");
  if (parts.length > 0) {
    bodyRows.push((narrow ? parts.slice(0, 1) : parts).join(" · "));
  }
  if (hint) {
    // 最窄的终端上只保留「/help」，避免裁完只剩省略号。
    bodyRows.push(narrow && visibleWidth(hint) > width ? "/help" : hint);
  }

  // ---- 第二遍：逐行裁剪到终端宽度、上色、按终端宽度居中 --------------------
  const lines = [""];
  const pushLine = (plain, style) => {
    const clipped =
      visibleWidth(plain) <= width ? plain : clipToWidth(plain, Math.max(1, width - 1));
    const w = visibleWidth(clipped);
    const pad = w >= width ? "" : " ".repeat(Math.floor((width - w) / 2));
    lines.push(pad + style(clipped));
  };

  titleRows.forEach((row, index) => {
    pushLine(row, (text) => {
      if (index === 0 && text.startsWith(brand)) {
        return (
          t.bold(gradientText(brand, t.palette ?? {}, t)) +
          t.dim(text.slice(brand.length))
        );
      }
      return t.dim(text);
    });
  });
  bodyRows.forEach((row) => pushLine(row, (text) => t.dim(text)));
  lines.push("");
  return `${lines.join("\n")}\n`;
}
