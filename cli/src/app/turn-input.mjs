// 回合内纠偏输入（M5c）：纯状态机，不碰 TTY、不挂网络，便于单测。
//
// cli.mjs 在 raw 模式下把 keypress 事件喂给 handleKey()，按返回的 action 分派：
//   submit → 把 text 发到当前回合（POST /api/v1/chat/steer）；abort → 中断回合；
//   edit   → 重绘底部 `steer> …` 提示；none → 无事可做。
//
// 缓冲上限按 UTF-8 字节计（默认 2048）：放不下的输入整体忽略（不截半个字符），
// 等 Backspace/Ctrl+U 腾出空间后再接受。

export const TurnInputAction = Object.freeze({
  Submit: "submit",
  Abort: "abort",
  Edit: "edit",
  None: "none",
});

export const TURN_INPUT_MAX_BYTES = 2048;

const encoder = new TextEncoder();
// 可打印文本：允许宽字符/emoji/空格，拒绝控制字符（\x00-\x1f、\x7f）。
const PRINTABLE_RE = /^[^\p{Cc}]+$/u;

export class TurnInputBuffer {
  #buffer = "";
  #maxBytes;

  constructor({ maxBytes = TURN_INPUT_MAX_BYTES } = {}) {
    this.#maxBytes =
      Number.isFinite(maxBytes) && maxBytes > 0 ? Math.floor(maxBytes) : TURN_INPUT_MAX_BYTES;
  }

  buffer() {
    return this.#buffer;
  }

  isEmpty() {
    return this.#buffer === "";
  }

  // 主动复位（回合结束/按键中断后的兜底）。
  clear() {
    this.#buffer = "";
  }

  // 处理一个 keypress 事件，返回 { action, text? }；text 只在 submit 时出现。
  handleKey(text, key = {}) {
    const name = key?.name ?? "";
    // Esc / Ctrl+C：中断回合（保持 cli.mjs 既有行为）；缓冲随回合作废。
    if (name === "escape" || (key?.ctrl && name === "c")) {
      this.#buffer = "";
      return { action: TurnInputAction.Abort };
    }
    // Ctrl+U：清空缓冲。
    if (key?.ctrl && name === "u") {
      const changed = this.#buffer !== "";
      this.#buffer = "";
      return { action: changed ? TurnInputAction.Edit : TurnInputAction.None };
    }
    // Enter：把缓冲作为消息返回并清空；空缓冲也返回 submit，由调用方决定是否发送。
    if (name === "return" || name === "enter") {
      const message = this.#buffer;
      this.#buffer = "";
      return { action: TurnInputAction.Submit, text: message };
    }
    // Backspace：删掉最后一个 code point（emoji 等代理对整体删除，不留半个字符）。
    if (name === "backspace") {
      if (this.#buffer === "") {
        return { action: TurnInputAction.None };
      }
      const chars = Array.from(this.#buffer);
      chars.pop();
      this.#buffer = chars.join("");
      return { action: TurnInputAction.Edit };
    }
    // 可打印字符（含整段粘贴）；组合键与控制字符忽略。
    if (
      typeof text !== "string" ||
      text === "" ||
      key?.ctrl ||
      key?.meta ||
      !PRINTABLE_RE.test(text)
    ) {
      return { action: TurnInputAction.None };
    }
    if (encoder.encode(this.#buffer + text).length > this.#maxBytes) {
      // 放不下就整体忽略（缓冲保持原样），避免截出半个字符。
      return { action: TurnInputAction.None };
    }
    this.#buffer += text;
    return { action: TurnInputAction.Edit };
  }
}
