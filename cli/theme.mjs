// 语义化主题令牌 + 终端能力检测 + 渲染帮助函数（零依赖）。
//
// 设计目标：
//   - 组件只使用语义色名（accent/success/error/...），不写死 ANSI；
//   - 按终端能力自动降级：truecolor -> 256 -> none（非 TTY / NO_COLOR / dumb）；
//   - 支持 dark/light 两套调色板，可在运行时切换（见 makeStyler 的可变 styler）；
//   - 统一图标，支持 ASCII / Nerd Font 回退。

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
  // 用户消息底色（对齐 pi 的 userMsgBg #343541）：发送后的输入用整块底色区分。
  userBg: [52, 53, 65],
};

// light 主题：适配浅色背景，整体更深更饱和。
const LIGHT = {
  accent: [43, 108, 106],
  border: [60, 90, 180],
  borderMuted: [170, 170, 170],
  success: [70, 120, 50],
  error: [180, 55, 55],
  warning: [160, 110, 0],
  muted: [110, 110, 110],
  dim: [140, 140, 140],
  text: [40, 40, 40],
  cyan: [0, 130, 160],
  magenta: [150, 80, 180],
  blue: [60, 90, 180],
  mdHeading: [160, 110, 0],
  mdCode: [43, 108, 106],
  diffAdded: [70, 120, 50],
  diffRemoved: [180, 55, 55],
  userBg: [232, 232, 232],
};

export const PALETTES = { dark: DARK, light: LIGHT };

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

// 构造一组渲染函数。返回的 styler 是可变的：可 setMode/setPalette，
// 以便共享同一实例的 renderer / editor / markdown 一起换主题。
export function makeStyler(mode = "none", palette = DARK) {
  const state = { mode, palette };
  const wrap = (code) => (text) => (state.mode === "none" ? text : `\x1b[${code}m${text}${RESET}`);
  const colorize = (name, text, isBg) => {
    if (state.mode === "none") {
      return text;
    }
    const rgb = state.palette[name];
    if (!rgb) {
      return text;
    }
    if (state.mode === "truecolor") {
      const [r, g, b] = rgb;
      return `\x1b[${isBg ? 48 : 38};2;${r};${g};${b}m${text}${RESET}`;
    }
    const index = rgbTo256(rgb[0], rgb[1], rgb[2]);
    return `\x1b[${isBg ? 48 : 38};5;${index}m${text}${RESET}`;
  };
  return {
    get mode() {
      return state.mode;
    },
    setMode(nextMode) {
      state.mode = nextMode;
    },
    get palette() {
      return state.palette;
    },
    setPalette(nextPalette) {
      state.palette = nextPalette;
    },
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

// Nerd Font 图标（需要安装 Nerd Font 才能正确显示）。
export const NERD_SYMBOLS = {
  ...SYMBOLS,
  ok: "\uf00c",
  fail: "\uf00d",
  warn: "\uf071",
  bullet: "\uf0da",
  interrupted: "\uf04d",
  compact: "\uf0ec",
};

export function pickSymbols(env = process.env) {
  if (env && env.DE_ASCII) {
    return ASCII_SYMBOLS;
  }
  if (env && (env.DE_NERD_FONT === "1" || env.DE_NERD_FONT === "true")) {
    return NERD_SYMBOLS;
  }
  return SYMBOLS;
}

// OSC 8 超链接：终端支持时可点击打开（默认用 file:// 指向本地文件）。
export function hyperlink(url, text) {
  return `\x1b]8;;${url}\x07${text}\x1b]8;;\x07`;
}
