const reset = "\x1b[0m";
const bold = "\x1b[1m";
const dim = "\x1b[2m";
const gray = "\x1b[90m";
const red = "\x1b[31m";
const green = "\x1b[32m";
const yellow = "\x1b[33m";
const blue = "\x1b[34m";
const magenta = "\x1b[35m";
const cyan = "\x1b[36m";

const MaxDisplayText = 4000;

// 等待动画：只在 TTY 上绘制，用 \r + 清行避免污染流式输出。
const SPINNER_FRAMES = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"];
const SPINNER_INTERVAL_MS = 100;
// 流式输出之间的静默阈值：超过它才显示 spinner，避免逐 token 闪烁。
const IDLE_DELAY_MS = 500;

function truncate(text) {
  if (text.length <= MaxDisplayText) {
    return text;
  }
  return `${text.slice(0, MaxDisplayText)}\n… (truncated ${text.length - MaxDisplayText} chars)`;
}

function prettyArguments(rawArguments) {
  if (rawArguments === undefined || rawArguments === null || rawArguments === "") {
    return "";
  }
  if (typeof rawArguments === "string") {
    try {
      return JSON.stringify(JSON.parse(rawArguments));
    } catch {
      return rawArguments;
    }
  }
  if (typeof rawArguments === "object") {
    return JSON.stringify(rawArguments);
  }
  return String(rawArguments);
}

class Spinner {
  #out;
  #timer = null;
  #idleTimer = null;
  #frame = 0;
  #label = "";
  #started = 0;
  #active = false;

  constructor(out) {
    this.#out = out;
  }

  get active() {
    return this.#active;
  }

  start(label) {
    if (!this.#out.isTTY) {
      return;
    }
    if (this.#active && this.#label === label) {
      this.#render();
      return;
    }
    this.#label = label;
    this.#started = Date.now();
    this.#frame = 0;
    if (!this.#active) {
      this.#active = true;
      this.#timer = setInterval(() => {
        this.#frame = (this.#frame + 1) % SPINNER_FRAMES.length;
        this.#render();
      }, SPINNER_INTERVAL_MS);
      this.#timer.unref?.();
    }
    this.#render();
  }

  // 延迟显示：静默 IDLE_DELAY_MS 后再出现。
  schedule(label) {
    if (!this.#out.isTTY) {
      return;
    }
    this.cancelScheduled();
    this.#idleTimer = setTimeout(() => {
      this.#idleTimer = null;
      this.start(label);
    }, IDLE_DELAY_MS);
    this.#idleTimer.unref?.();
  }

  cancelScheduled() {
    if (this.#idleTimer) {
      clearTimeout(this.#idleTimer);
      this.#idleTimer = null;
    }
  }

  stop() {
    this.cancelScheduled();
    if (!this.#active) {
      return;
    }
    this.#active = false;
    if (this.#timer) {
      clearInterval(this.#timer);
      this.#timer = null;
    }
    // 清掉当前行，避免残留。
    this.#out.write("\r\x1b[K");
  }

  #render() {
    if (!this.#active) {
      return;
    }
    const seconds = Math.floor((Date.now() - this.#started) / 1000);
    const elapsed = seconds > 0 ? ` ${seconds}s` : "";
    this.#out.write(
      `\r\x1b[K${dim}${SPINNER_FRAMES[this.#frame]} ${this.#label}${elapsed}${reset}`,
    );
  }
}

export class TranscriptRenderer {
  #out = process.stdout;
  #assistantActive = false;
  #assistantWroteText = false;
  #resumeAssistant = false;
  #spinner;
  #currentTool = "";

  constructor(out = process.stdout) {
    this.#out = out;
    this.#spinner = new Spinner(out);
  }

  // 是否正在显示等待动画（测试用）。
  get waiting() {
    return this.#spinner.active;
  }

  // 供非流式长耗时操作（如 /compact）使用的外部等待接口。
  wait(label = "working") {
    this.#beginOutput();
    this.#spinner.start(label);
  }

  stopWaiting() {
    this.#beginOutput();
  }

  user(text) {
    this.#beginOutput();
    this.#write(`\n${bold}${blue}user${reset} ${text}\n`);
    this.#assistantActive = false;
    this.#assistantWroteText = false;
    this.#resumeAssistant = false;
  }

  startReply() {
    this.#beginOutput();
    this.#write(`\n${bold}${magenta}assistant${reset} `);
    this.#assistantActive = true;
    this.#assistantWroteText = false;
    this.#resumeAssistant = false;
    this.#spinner.start("thinking");
  }

  content(token) {
    this.#beginOutput();
    if (!this.#assistantActive) {
      this.#write(`\n${bold}${magenta}assistant${reset} `);
      this.#assistantActive = true;
    }
    this.#assistantWroteText = true;
    this.#resumeAssistant = false;
    this.#write(token);
    // token 之间若长时间无输出，再显示等待动画。
    this.#spinner.schedule("thinking");
  }

  toolStart(id, name, rawArguments) {
    this.#beginOutput();
    this.#write(`\n\n${bold}${cyan}▸ ${name}${reset}`);
    const args = prettyArguments(rawArguments);
    if (args) {
      this.#write(` ${dim}${args}${reset}`);
    }
    if (id) {
      this.#write(` ${gray}(${id.slice(0, 12)})${reset}`);
    }
    this.#write("\n");
    this.#assistantActive = false;
    this.#assistantWroteText = false;
    this.#resumeAssistant = true;
    this.#currentTool = name;
    this.#spinner.start(`${name} running`);
  }

  toolOutput(text) {
    this.#beginOutput();
    const indented = truncate(text).replace(/\n/g, "\n  ");
    this.#write(`${dim}  ${indented}${reset}\n`);
    if (this.#currentTool) {
      this.#spinner.start(`${this.#currentTool} running`);
    }
  }

  toolEnd(ok, text) {
    this.#beginOutput();
    if (ok) {
      this.#write(`${green}✔ ok${reset}`);
    } else {
      this.#write(`${red}✘ failed${reset}`);
    }
    if (text && !ok) {
      this.#write(` ${red}${truncate(text).slice(0, 500)}${reset}`);
    } else if (text && ok) {
      this.#write(`\n${dim}${truncate(text)}${reset}`);
    }
    this.#write("\n");
    this.#resumeAssistant = true;
    this.#currentTool = "";
    // 工具结束后等模型继续生成。
    this.#spinner.start("thinking");
  }

  endReply() {
    this.#beginOutput();
    this.#write("\n");
    this.#assistantActive = false;
    this.#assistantWroteText = false;
    this.#resumeAssistant = false;
  }

  error(message) {
    this.#beginOutput();
    this.#write(`\n${red}error: ${message}${reset}\n`);
    this.endReply();
  }

  interrupted() {
    this.#beginOutput();
    this.#write(`\n${yellow}⏹ interrupted${reset}\n`);
    this.#assistantActive = false;
    this.#assistantWroteText = false;
    this.#resumeAssistant = false;
  }

  approvalRequest(data) {
    this.#beginOutput();
    const name = data?.name ?? "tool";
    const reason = data?.reason ?? "requires approval";
    const args = prettyArguments(data?.arguments);
    this.#write(`\n${bold}${yellow}⚠ approval needed${reset} ${bold}${name}${reset}`);
    if (args) {
      this.#write(` ${dim}${args}${reset}`);
    }
    this.#write(`\n${dim}  ${reason}${reset}\n`);
    this.#assistantActive = false;
    this.#assistantWroteText = false;
    this.#resumeAssistant = true;
  }

  approvalResolved(decision) {
    this.#beginOutput();
    const label =
      decision === "allow_once"
        ? `${green}approved (once)${reset}`
        : decision === "allow_session"
          ? `${green}approved (this session)${reset}`
          : `${red}denied${reset}`;
    this.#write(`${dim}  → ${label}\n`);
    this.#resumeAssistant = true;
    this.#spinner.start("thinking");
  }

  compaction(data) {
    this.#beginOutput();
    const before = data?.tokens_before ?? "?";
    const after = data?.tokens_after ?? "?";
    const reason = data?.forced ? " (overflow)" : "";
    this.#write(
      `\n${dim}⤵ context compacted${reason}: ${before} → ${after} tokens${reset}\n`,
    );
    this.#assistantActive = false;
    this.#assistantWroteText = false;
    this.#resumeAssistant = true;
    this.#spinner.start("thinking");
  }

  // 任何可见输出前调用：取消待显示的 spinner 并清掉当前等待行。
  #beginOutput() {
    this.#spinner.stop();
  }

  #write(text) {
    this.#out.write(text);
  }
}
