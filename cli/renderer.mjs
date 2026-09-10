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

export class TranscriptRenderer {
  #out = process.stdout;
  #assistantActive = false;
  #assistantWroteText = false;
  #resumeAssistant = false;

  constructor(out = process.stdout) {
    this.#out = out;
  }

  user(text) {
    this.#write(`\n${bold}${blue}user${reset} ${text}\n`);
    this.#assistantActive = false;
    this.#assistantWroteText = false;
    this.#resumeAssistant = false;
  }

  startReply() {
    this.#write(`\n${bold}${magenta}assistant${reset} `);
    this.#assistantActive = true;
    this.#assistantWroteText = false;
    this.#resumeAssistant = false;
  }

  content(token) {
    if (!this.#assistantActive) {
      this.#write(`\n${bold}${magenta}assistant${reset} `);
      this.#assistantActive = true;
    }
    this.#assistantWroteText = true;
    this.#resumeAssistant = false;
    this.#write(token);
  }

  toolStart(id, name, rawArguments) {
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
  }

  toolOutput(text) {
    const indented = truncate(text).replace(/\n/g, "\n  ");
    this.#write(`${dim}  ${indented}${reset}\n`);
  }

  toolEnd(ok, text) {
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
  }

  endReply() {
    this.#write("\n");
    this.#assistantActive = false;
    this.#assistantWroteText = false;
    this.#resumeAssistant = false;
  }

  error(message) {
    this.#write(`\n${red}error: ${message}${reset}\n`);
    this.endReply();
  }

  interrupted() {
    this.#write(`\n${yellow}⏹ interrupted${reset}\n`);
    this.#assistantActive = false;
    this.#assistantWroteText = false;
    this.#resumeAssistant = false;
  }

  compaction(data) {
    const before = data?.tokens_before ?? "?";
    const after = data?.tokens_after ?? "?";
    const reason = data?.forced ? " (overflow)" : "";
    this.#write(
      `\n${dim}⤵ context compacted${reason}: ${before} → ${after} tokens${reset}\n`,
    );
    this.#assistantActive = false;
    this.#assistantWroteText = false;
    this.#resumeAssistant = true;
  }

  #write(text) {
    this.#out.write(text);
  }
}
