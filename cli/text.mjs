// 终端文本工具：ANSI 感知的宽度计算、裁剪与折行（零依赖）。
//
// 背景：底部区域（输入框 / 状态行 / 未完成输出行）里的字符串大多已经上色。
// 一旦按 code unit 或把转义序列也当字符来算宽度，擦除与光标定位就会错位
// （表现为残影、光标跳动）。这里统一按「可见列」计算。
//
// 约定：
//   - CSI（ESC [ ... 终止字母）与 OSC（ESC ] ... BEL / ESC \）视为零宽；
//   - 其余字符按东亚宽度规则计宽（全角 2 列，控制字符 0 列）。

const CSI_PATTERN = /^\x1b\[[0-9;?]*[ -/]*[@-~]/;
const OSC_PATTERN = /^\x1b\][\s\S]*?(?:\x07|\x1b\\)/;

// 粗略的东亚宽字符判定。指令名/路径基本都是 ASCII，这里主要保证中文输入与
// 中文回复不错位。
export function charWidth(codePoint) {
  if (codePoint === 0) {
    return 0;
  }
  if (codePoint < 32 || (codePoint >= 0x7f && codePoint < 0xa0)) {
    return 0;
  }
  if (
    (codePoint >= 0x1100 && codePoint <= 0x115f) ||
    codePoint === 0x2329 ||
    codePoint === 0x232a ||
    (codePoint >= 0x2e80 && codePoint <= 0x303e) ||
    (codePoint >= 0x3041 && codePoint <= 0x33ff) ||
    (codePoint >= 0x3400 && codePoint <= 0x4dbf) ||
    (codePoint >= 0x4e00 && codePoint <= 0x9fff) ||
    (codePoint >= 0xa000 && codePoint <= 0xa4cf) ||
    (codePoint >= 0xac00 && codePoint <= 0xd7a3) ||
    (codePoint >= 0xf900 && codePoint <= 0xfaff) ||
    (codePoint >= 0xfe10 && codePoint <= 0xfe19) ||
    (codePoint >= 0xfe30 && codePoint <= 0xfe6f) ||
    (codePoint >= 0xff00 && codePoint <= 0xff60) ||
    (codePoint >= 0xffe0 && codePoint <= 0xffe6) ||
    (codePoint >= 0x1f300 && codePoint <= 0x1faff) ||
    (codePoint >= 0x20000 && codePoint <= 0x3fffd)
  ) {
    return 2;
  }
  return 1;
}

// 纯文本宽度（不含转义序列时等价于可见宽度）。
export function displayWidth(text) {
  let width = 0;
  for (const char of String(text)) {
    width += charWidth(char.codePointAt(0));
  }
  return width;
}

// 返回 text 在 index 处的转义序列长度（0 表示不是转义序列）。
function escapeAt(text, index) {
  if (text[index] !== "\x1b") {
    return 0;
  }
  const match = CSI_PATTERN.exec(text.slice(index)) ?? OSC_PATTERN.exec(text.slice(index));
  return match ? match[0].length : 1;
}

export function stripAnsi(text) {
  const source = String(text ?? "");
  let out = "";
  let index = 0;
  while (index < source.length) {
    const escape = escapeAt(source, index);
    if (escape > 0) {
      index += escape;
      continue;
    }
    out += source[index];
    index += 1;
  }
  return out;
}

// 可见宽度：跳过 ANSI 序列后的列数。
export function visibleWidth(text) {
  const source = String(text ?? "");
  let width = 0;
  let index = 0;
  while (index < source.length) {
    const escape = escapeAt(source, index);
    if (escape > 0) {
      index += escape;
      continue;
    }
    const codePoint = source.codePointAt(index);
    const char = String.fromCodePoint(codePoint);
    width += charWidth(codePoint);
    index += char.length;
  }
  return width;
}

// 按可见宽度裁剪，保留（并原样复制）转义序列。默认在裁剪处补省略号。
export function clipVisible(text, maxWidth, { ellipsis = true } = {}) {
  const source = String(text ?? "");
  if (maxWidth <= 0) {
    return "";
  }
  let width = 0;
  let out = "";
  let index = 0;
  let clipped = false;
  let sawEscape = false;
  while (index < source.length) {
    const escape = escapeAt(source, index);
    if (escape > 0) {
      sawEscape = true;
      out += source.slice(index, index + escape);
      index += escape;
      continue;
    }
    const codePoint = source.codePointAt(index);
    const char = String.fromCodePoint(codePoint);
    const w = charWidth(codePoint);
    if (width + w > maxWidth) {
      clipped = true;
      break;
    }
    out += char;
    width += w;
    index += char.length;
  }
  if (clipped && ellipsis) {
    out += sawEscape ? "\x1b[0m…" : "…";
  }
  return out;
}

// 纯文本裁剪（保留原有语义：超宽时补省略号）。
export function clipToWidth(text, maxWidth) {
  const source = String(text ?? "");
  if (maxWidth <= 0) {
    return "";
  }
  let width = 0;
  let out = "";
  for (const char of source) {
    const w = charWidth(char.codePointAt(0));
    if (width + w > maxWidth) {
      return `${out}…`;
    }
    out += char;
    width += w;
  }
  return out;
}

// 按可见宽度折成多行（保留转义序列）。用于已上色的输出片段；
// 换行符由调用方先行处理（本函数不拆 \n）。
export function wrapVisible(text, width) {
  const source = String(text ?? "");
  const limit = Math.max(1, Math.floor(width));
  const rows = [];
  let current = "";
  let currentWidth = 0;
  let index = 0;
  while (index < source.length) {
    const escape = escapeAt(source, index);
    if (escape > 0) {
      current += source.slice(index, index + escape);
      index += escape;
      continue;
    }
    const codePoint = source.codePointAt(index);
    const char = String.fromCodePoint(codePoint);
    const w = charWidth(codePoint);
    if (currentWidth + w > limit) {
      rows.push(current);
      current = "";
      currentWidth = 0;
    }
    current += char;
    currentWidth += w;
    index += char.length;
  }
  rows.push(current);
  return rows;
}
