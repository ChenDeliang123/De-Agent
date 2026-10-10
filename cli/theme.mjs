// 语义化主题令牌 + 终端能力检测 + 渲染帮助函数（零依赖）。
//
// 设计目标：
//   - 组件只使用语义色名（accent/success/error/...），不写死 ANSI；
//   - 按终端能力自动降级：truecolor -> 256 -> none（非 TTY / NO_COLOR / dumb）；
//   - 统一图标，便于将来做 ASCII 回退。

const RESET = "\x1b[0m";

// dark 主题（RGB）。参考 pi 的 dark.json 语义色。
const DARK = {
  accent: [138, 190, 183],
  border: [95, 135, 255],
  borderMuted: [80, 80, 80],
  success: [181, 189, 104],
  error: [204, 102, 102],
  warning: [230, 200, 90],
  muted: [128, 128, 128],
  dim: [102, 102, 102],
  text: [212, 212, 212],
  cyan: [0, 215, 255],
  magenta: [198, 120, 221],
  blue: [95, 135, 255],
  mdHeading: [240, 198, 116],
  mdCode: [138, 190, 183],
  diffAdded: [181, 189, 104],
  diffRemoved: [204, 102, 102],
};

function rgbTo256(r, g, b) {
  if (Math.abs(r - g) < 10 && Math.abs(g - b) < 10) {
    if (r < 8) return 16;
    if (r > 248) return 231;
    return 232 + Math.round(((r - 8) / 247) * 24);
  }
  const channel = (value) => Math.round((value / 255) * 5);
  return 16 + 36 * channel(r) + 6 * channel(g) + channel(b);
}

// 判定颜色能力。none 表示输出不带任何 ANSI。
export function detectColorMode(env = process.env, stream = process.stdout) {
  if (env && env.NO_COLOR) {
    return "none";
  }
  if (!stream || stream.isTTY !== true) {
    return "none";
  }
  if (env && env.TERM === "dumb") {
    return "none";
  }
  if (env && /^(truecolor|24bit)$/i.test(env.COLORTERM || "")) {
    return "truecolor";
  }
  return "256";
}

// 构造一组渲染函数；mode 为 none 时全部退化为原样返回。
export function makeStyler(mode = "none", palette = DARK) {
  const wrap = (code) => (text) => (mode === "none" ? text : `\x1b[${code}m${text}${RESET}`);
  const colorize = (name, text, isBg) => {
    if (mode === "none") {
      return text;
    }
    const rgb = palette[name];
    if (!rgb) {
      return text;
    }
    if (mode === "truecolor") {
      const [r, g, b] = rgb;
      return `\x1b[${isBg ? 48 : 38};2;${r};${g};${b}m${text}${RESET}`;
    }
    const index = rgbTo256(rgb[0], rgb[1], rgb[2]);
    return `\x1b[${isBg ? 48 : 38};5;${index}m${text}${RESET}`;
  };
  return {
    mode,
    fg: (name, text) => colorize(name, text, false),
    bg: (name, text) => colorize(name, text, true),
    bold: wrap(1),
    dim: wrap(2),
    italic: wrap(3),
    inverse: wrap(7),
    plain: (text) => text,
  };
}

export const SYMBOLS = {
  prompt: "❯",
  gutter: "▌",
  bar: "┃",
  top: "┏",
  bottom: "┗",
  ok: "✔",
  fail: "✘",
  warn: "⚠",
  bullet: "▸",
  interrupted: "⏹",
  compact: "⤵",
  dot: "·",
};

export const ASCII_SYMBOLS = {
  prompt: ">",
  gutter: "|",
  bar: "|",
  top: "+",
  bottom: "+",
  ok: "ok",
  fail: "x",
  warn: "!",
  bullet: ">",
  interrupted: "!",
  compact: "^",
  dot: "-",
};

export function pickSymbols(env = process.env) {
  return env && env.DE_ASCII ? ASCII_SYMBOLS : SYMBOLS;
}
