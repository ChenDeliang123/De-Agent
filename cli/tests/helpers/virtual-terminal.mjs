// 极简 ANSI 终端模拟器：把渲染器写出的转义序列还原成一份屏幕快照，
// 用于断言重绘后不残留旧行（纯字符串断言看不出这类问题）。
export class VirtualTerminal {
  constructor() {
    this.rows = [];
    this.row = 0;
    this.col = 0;
  }

  #ensureRow(index) {
    while (this.rows.length <= index) {
      this.rows.push([]);
    }
  }

  #writeChar(ch) {
    this.#ensureRow(this.row);
    const row = this.rows[this.row];
    while (row.length < this.col) {
      row.push(" ");
    }
    row[this.col] = ch;
    this.col += 1;
  }

  write(text) {
    for (let i = 0; i < text.length; i += 1) {
      const ch = text[i];
      if (ch === "\x1b") {
        // OSC（超链接等）：ESC ] ... BEL 或 ESC \ —— 零宽，直接跳过。
        if (text[i + 1] === "]") {
          const end = text.slice(i).search(/\x07|\x1b\\/);
          if (end !== -1) {
            const terminator = text[i + end] === "\x07" ? 1 : 2;
            i += end + terminator - 1;
            continue;
          }
        }
        const match = /^\x1b\[([0-9;]*)([A-Za-z])/.exec(text.slice(i));
        if (match) {
          const count = match[1] === "" ? 1 : Number(match[1]);
          const code = match[2];
          if (code === "A") {
            this.row = Math.max(0, this.row - count);
          } else if (code === "B") {
            this.row += count;
          } else if (code === "C") {
            this.col += count;
          } else if (code === "D") {
            this.col = Math.max(0, this.col - count);
          } else if (code === "K") {
            this.#ensureRow(this.row);
            this.rows[this.row] = this.rows[this.row].slice(0, this.col);
          }
          i += match[0].length - 1;
          continue;
        }
      }
      if (ch === "\r") {
        this.col = 0;
      } else if (ch === "\n") {
        this.row += 1;
        this.col = 0;
      } else {
        this.#writeChar(ch);
      }
    }
  }

  lines() {
    return this.rows.map((row) => row.join("").replace(/\s+$/, ""));
  }

  // 去掉尾部空行后的屏幕内容（更接近肉眼看到的样子）。
  screen() {
    const lines = this.lines();
    while (lines.length > 0 && lines[lines.length - 1] === "") {
      lines.pop();
    }
    return lines;
  }
}

// 把写出的分片接到虚拟终端上，返回 { terminal, write, text }。
export function captureOutput(columns = 80) {
  const terminal = new VirtualTerminal();
  const chunks = [];
  const out = {
    isTTY: true,
    columns,
    write(text) {
      chunks.push(text);
      terminal.write(text);
      return true;
    },
  };
  return { terminal, out, text: () => chunks.join("") };
}
