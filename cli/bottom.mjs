// 底部区域渲染器：把「状态行 + 输入框」固定在终端最后几行，
// 并让状态行在流式输出滚动时保持常驻。
//
// 为什么不照搬 pi 的行级 diff 渲染？
//   pi 用完整 TUI 视口 + 行级 diff（tui-main-screen.ts）来维持底部输入框与 footer。
//   本项目是行式终端 + raw-mode 输入行，区域只有「输入框（3~10 行）+ 状态行（1 行）」，
//   整区重绘的代价可以接受，逻辑简单且可测；代价是高频重绘时可能有轻微闪烁。
//
// 关键不变量：
//   1. 区域始终是「连续的行块」，且光标始终停在区域内 —— 这样擦除/重绘只需相对移动；
//   2. 区域内每一行都按可见宽度裁剪到 contentWidth（= columns - 1），
//      保证终端不会自动折行，物理行数与我们记账一致；
//   3. 静态输出（聊天内容）通过 appendOutput 写入：先擦除区域 → 写完整行 → 重绘区域。

import { clipVisible, visibleWidth, wrapVisible } from "./text.mjs";

export class BottomRegion {
  #out;
  #enabled;
  #rows = 0;
  #cursorRow = 0;
  #suspended = false;
  #editor = null;
  #footer = "";
  #working = "";
  #pending = "";

  constructor(output, { enabled } = {}) {
    this.#out = output;
    this.#enabled = enabled ?? output?.isTTY === true;
  }

  get enabled() {
    return this.#enabled && Boolean(this.#out);
  }

  get suspended() {
    return this.#suspended;
  }

  // 已绘制的物理行数（供测试断言）。
  get rows() {
    return this.#rows;
  }

  // 区域内可用的内容宽度：留 1 列，避免写满最后一行触发终端的 pending-wrap。
  get contentWidth() {
    return Math.max(8, this.#columns() - 1);
  }

  get pending() {
    return this.#pending;
  }

  // 底部状态行的信息段（cwd/分支/模式/会话），由调用方按主题上色。
  setFooter(text) {
    this.#footer = String(text ?? "");
    this.render();
  }

  // 工作状态（spinner + 文案），显示在状态行最左侧；空串表示空闲。
  setWorking(text) {
    this.#working = String(text ?? "");
    this.render();
  }

  // 输入框块：{ lines, cursorRow, cursorCol }；null 表示收起输入框（保留状态行）。
  setEditor(block) {
    this.#editor = block ?? null;
    this.render();
  }

  // 未换行的输出片段：留在区域顶部，等换行后才落到上方滚动区。
  setPending(text) {
    this.#pending = String(text ?? "");
    this.render();
  }

  #columns() {
    const columns = Number(this.#out?.columns);
    return Number.isFinite(columns) && columns > 0 ? Math.floor(columns) : 80;
  }

  #statusRow() {
    const parts = [];
    if (this.#working) {
      parts.push(this.#working);
    }
    if (this.#footer) {
      parts.push(this.#footer);
    }
    return clipVisible(parts.join("  "), this.contentWidth, { ellipsis: false });
  }

  #compose() {
    const width = this.contentWidth;
    const rows = [];
    let cursorRow = 0;
    let cursorCol = 0;

    if (this.#pending) {
      const wrapped = wrapVisible(this.#pending, width);
      for (const row of wrapped) {
        rows.push(row);
      }
      cursorRow = rows.length - 1;
      cursorCol = visibleWidth(wrapped[wrapped.length - 1]);
    }

    if (this.#editor) {
      const base = rows.length;
      const lines = Array.isArray(this.#editor.lines) ? this.#editor.lines : [];
      for (const line of lines) {
        rows.push(clipVisible(line, width, { ellipsis: false }));
      }
      cursorRow = base + (this.#editor.cursorRow ?? 0);
      cursorCol = this.#editor.cursorCol ?? 0;
    } else if (!this.#pending) {
      // 只有状态行：光标停在状态行末尾所在行的行首（写入前会先擦除）。
      cursorRow = rows.length;
    }

    const status = this.#statusRow();
    if (status) {
      rows.push(status);
      if (!this.#pending && !this.#editor) {
        cursorCol = visibleWidth(status);
      }
    }

    return {
      rows,
      cursorRow: Math.max(0, Math.min(cursorRow, Math.max(0, rows.length - 1))),
      cursorCol: Math.max(0, Math.min(cursorCol, width)),
    };
  }

  // 擦除已绘制区域，并把光标留在区域首行行首。约定：调用时光标位于区域内。
  #eraseFrame() {
    if (this.#rows <= 0) {
      return "";
    }
    let frame = "";
    if (this.#cursorRow > 0) {
      frame += `\x1b[${this.#cursorRow}A`;
    }
    for (let i = 0; i < this.#rows; i += 1) {
      frame += "\r\x1b[K";
      if (i < this.#rows - 1) {
        frame += "\x1b[1B";
      }
    }
    if (this.#rows > 1) {
      frame += `\x1b[${this.#rows - 1}A`;
    }
    this.#cursorRow = 0;
    return frame;
  }

  render() {
    if (!this.enabled || this.#suspended) {
      return;
    }
    const { rows, cursorRow, cursorCol } = this.#compose();
    let frame = this.#eraseFrame();
    for (let i = 0; i < rows.length; i += 1) {
      frame += i === 0 ? `\r${rows[i]}` : `\r\n${rows[i]}`;
    }
    const lastRow = rows.length - 1;
    if (lastRow > cursorRow) {
      frame += `\x1b[${lastRow - cursorRow}A`;
    }
    frame += "\r";
    if (cursorCol > 0) {
      frame += `\x1b[${cursorCol}C`;
    }
    this.#rows = rows.length;
    this.#cursorRow = rows.length > 0 ? cursorRow : 0;
    this.#out.write(frame);
  }

  // 擦除区域并遗忘记账（用于外部清屏前/退出前）。
  erase() {
    if (!this.enabled) {
      this.#rows = 0;
      return;
    }
    this.#out.write(this.#eraseFrame());
    this.#rows = 0;
  }

  // 外部清屏（\x1b[2J）后调用：旧区域已不存在，只需重置记账并重绘。
  reset() {
    this.#rows = 0;
    this.#cursorRow = 0;
  }

  // 挂起：擦除区域并把未完成的行落盘，把终端让给外部程序/交互提示。
  suspend() {
    this.#suspended = true;
    if (!this.enabled) {
      this.#rows = 0;
      return;
    }
    const pending = this.#pending;
    this.#pending = "";
    let frame = this.#eraseFrame();
    if (pending) {
      frame += `${pending}\r\n`;
    }
    this.#rows = 0;
    this.#out.write(frame);
  }

  resume() {
    this.#suspended = false;
    this.render();
  }

  // 输出静态文本（聊天内容、命令输出）。区域存在时保持状态行常驻在底部。
  appendOutput(text) {
    const chunk = String(text ?? "");
    if (!this.enabled || this.#suspended) {
      this.#out?.write(chunk);
      return;
    }
    const combined = this.#pending + chunk;
    const lines = combined.split("\n");
    this.#pending = lines.pop() ?? "";
    let frame = this.#eraseFrame();
    this.#rows = 0;
    for (const line of lines) {
      // 用 \r\n：raw 模式下也不会丢失回车（canonical 模式下多一个 \r 无害）。
      frame += `${line}\r\n`;
    }
    this.#out.write(frame);
    this.render();
  }
}
