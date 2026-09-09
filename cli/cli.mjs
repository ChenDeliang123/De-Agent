import readlinePromises from "node:readline/promises";
import { stdin as input, stdout as output } from "node:process";

import { TranscriptRenderer } from "./renderer.mjs";
import { log } from "./logger.mjs";
import { listSessions, loadHistory, streamChat } from "./session.mjs";

const defaultBackend = "http://127.0.0.1:18081";
const renderer = new TranscriptRenderer();

function parseArgs(argv) {
  const options = { backend: defaultBackend, session: null };
  for (let i = 0; i < argv.length; i += 1) {
    if (argv[i] === "--backend" && i + 1 < argv.length) {
      options.backend = argv[++i];
    } else if (argv[i] === "--session" && i + 1 < argv.length) {
      options.session = argv[++i];
    } else if (argv[i] === "--help" || argv[i] === "-h") {
      options.help = true;
    }
  }
  return options;
}

function printHelp() {
  console.log(`
usage: node cli.mjs [--backend <url>] [--session <id>]

commands:
  /new           start a new session
  /sessions      list backend sessions
  /open <id>     resume a session by id or index
  /help          show this help
  /quit          exit
`);
}

function clearInputLine() {
  process.stdout.write("\r\x1b[K");
}

async function renderHistory(backend, sessionId) {
  const history = await loadHistory(backend, sessionId);
  log("info", "cli", `event=history_load session=${sessionId} messages=${history.length}`);
  for (const message of history) {
    if (message.role === "user") {
      renderer.user(message.content);
    } else if (message.role === "assistant") {
      renderer.startReply();
      renderer.content(message.content);
      renderer.endReply();
    }
  }
}

async function showSessions(backend) {
  const sessions = await listSessions(backend);
  if (sessions.length === 0) {
    console.log("(no sessions yet)");
    return null;
  }
  sessions.forEach((session, index) => {
    const id = typeof session === "string" ? session : session.id;
    const title = typeof session === "string" ? `session ${index}` : session.title || "untitled";
    console.log(`  ${index + 1}. ${title}  (${id})`);
  });
  return sessions;
}

async function handleAnswer(backend, state, answer) {
  const trimmed = answer.trim();
  if (!trimmed) {
    return false;
  }
  if (trimmed.startsWith("/")) {
    const [command, arg] = trimmed.split(/\s+/, 2);
    if (command === "/quit" || command === "/exit") {
      return true;
    }
    if (command === "/help") {
      printHelp();
    } else if (command === "/new") {
      state.sessionId = null;
      console.log("new session");
      log("info", "cli", "event=new_session");
    } else if (command === "/sessions") {
      await showSessions(backend);
    } else if (command === "/open") {
      const all = await listSessions(backend);
      let target = arg;
      if (!target && all.length > 0) {
        target = String(all.length);
      }
      if (!target) {
        console.log("no sessions to open");
        return false;
      }
      const numeric = Number(target);
      const entry =
        Number.isInteger(numeric) && numeric >= 1 && numeric <= all.length
          ? all[numeric - 1]
          : all.find(
              (session) => (typeof session === "string" ? session : session.id) === target,
            );
      if (!entry) {
        console.log(`session not found: ${target}`);
        return false;
      }
      state.sessionId = typeof entry === "string" ? entry : entry.id;
      clearInputLine();
      await renderHistory(backend, state.sessionId);
      log("info", "cli", `event=open_session session=${state.sessionId}`);
    } else {
      console.log(`unknown command: ${command}; use /help`);
    }
    return false;
  }

  state.sessionId = await sendMessage(backend, state.sessionId, trimmed);
  return false;
}

async function sendMessage(backend, sessionId, text) {
  clearInputLine();
  log("info", "cli", `event=user_message session=${sessionId ?? "-"} bytes=${text.length}`);
  renderer.user(text);
  renderer.startReply();
  try {
    const done = await streamChat(backend, sessionId, text, ({ event, data }) => {
      if (!data) {
        return;
      }
      if (event === "content") {
        renderer.content(data.content ?? "");
      } else if (event === "tool_start") {
        renderer.toolStart(data.id ?? "", data.name ?? "tool", data.arguments ?? "");
      } else if (event === "tool_output") {
        renderer.toolOutput(data.content ?? "");
      } else if (event === "tool_end") {
        renderer.toolEnd(Boolean(data.ok), data.text ?? "");
      } else if (event === "error") {
        throw new Error(data.error ?? "unknown error");
      }
    });
    renderer.endReply();
    log("info", "cli",
        `event=stream_end session=${done?.session_id ?? sessionId ?? "-"}`);
    return done?.session_id ?? sessionId;
  } catch (error) {
    renderer.error(error.message);
    log("error", "cli", `event=stream_error message=${error.message}`);
    return sessionId;
  }
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  if (options.help) {
    printHelp();
    return;
  }

  const backend = options.backend.replace(/\/+$/, "");
  console.log(`ai-agent-cli  backend=${backend}`);
  log("info", "cli", `event=cli_start backend=${backend}`);

  const state = { sessionId: options.session };
  const sessions = await listSessions(backend).catch(() => []);
  if (sessions.length > 0) {
    console.log(`found ${sessions.length} session(s); use /sessions or /open <id>`);
  }
  if (state.sessionId) {
    try {
      await renderHistory(backend, state.sessionId);
    } catch (error) {
      console.error(`cannot load session ${state.sessionId}: ${error.message}`);
      state.sessionId = null;
    }
  }

  try {
    if (input.isTTY) {
      const rl = readlinePromises.createInterface({ input, output, terminal: true });
      while (true) {
        const label = state.sessionId ? state.sessionId.slice(0, 8) : "new";
        const answer = await rl.question(`[${label}] ❯ `);
        const shouldExit = await handleAnswer(backend, state, answer);
        if (shouldExit) {
          break;
        }
      }
      rl.close();
    } else {
      const lines = readlinePromises.createInterface({ input, terminal: false });
      for await (const line of lines) {
        const shouldExit = await handleAnswer(backend, state, line);
        if (shouldExit) {
          break;
        }
      }
      lines.close();
    }
  } finally {
    clearInputLine();
  }
}

main().catch((error) => {
  clearInputLine();
  console.error(error);
  log("error", "cli", `event=cli_fatal message=${error.message}`);
  process.exitCode = 1;
});
