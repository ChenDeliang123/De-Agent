import readlinePromises from "node:readline/promises";
import path from "node:path";
import { randomUUID } from "node:crypto";
import { stdin as input, stdout as output } from "node:process";

import { TranscriptRenderer } from "./renderer.mjs";
import { log } from "./logger.mjs";
import { cancelChat, listSessions, loadHistory, renameSession, streamChat } from "./session.mjs";
import { LineEditor, renderHotkeys } from "./editor.mjs";
import { commands, completeCommands, renderHelp, runCommand } from "./commands.mjs";

const defaultBackend = "http://127.0.0.1:18081";
const renderer = new TranscriptRenderer();
let activeAbortController = null;

if (input.isTTY) {
  input.on("keypress", (_text, key) => {
    if (!activeAbortController) {
      return;
    }
    if (key?.name === "escape" || (key?.ctrl && key?.name === "c")) {
      activeAbortController.abort();
      activeAbortController = null;
    }
  });
}

function parseArgs(argv) {
  const options = {
    backend: defaultBackend,
    session: null,
    continueSession: false,
    resume: false,
    name: null,
    cwd: null,
    editor: null,
  };
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === "--backend" && i + 1 < argv.length) {
      options.backend = argv[++i];
    } else if (arg === "--session" && i + 1 < argv.length) {
      options.session = argv[++i];
    } else if (arg === "-c" || arg === "--continue") {
      options.continueSession = true;
    } else if (arg === "-r" || arg === "--resume") {
      options.resume = true;
    } else if (arg === "--name" && i + 1 < argv.length) {
      options.name = argv[++i];
    } else if (arg === "--cwd" && i + 1 < argv.length) {
      options.cwd = argv[++i];
    } else if (arg === "--editor" && i + 1 < argv.length) {
      options.editor = argv[++i];
    } else if (arg === "--help" || arg === "-h") {
      options.help = true;
    }
  }
  return options;
}

function printHelp() {
  console.log(`
usage: node cli.mjs [options]

options:
  --backend <url>   Web Gateway base url (default ${defaultBackend})
  --session <id>    resume a specific session
  -c, --continue    continue the most recent session
  -r, --resume      pick a session to resume at startup
  --name <name>     display name for the new session
  --cwd <dir>       tool working directory (default: current directory)
  --editor <cmd>    external editor for Ctrl+G (default: $AIAGENT_EDITOR/$VISUAL/$EDITOR)
  -h, --help        show this help
`);
  console.log(renderHelp(commands));
  console.log(`\n${renderHotkeys()}\n`);
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

function sessionEntryId(entry) {
  return typeof entry === "string" ? entry : entry.id;
}

function sessionEntryLabel(entry) {
  return typeof entry === "string" ? entry : entry.title || "untitled";
}

async function handleLine(ctx, answer) {
  const trimmed = answer.trim();
  if (!trimmed) {
    return;
  }
  if (trimmed.startsWith("/")) {
    await runCommand(ctx, trimmed);
    return;
  }
  ctx.state.sessionId = await sendMessage(ctx, trimmed);
  if (ctx.state.pendingName) {
    try {
      await renameSession(ctx.backend, ctx.state.sessionId, ctx.state.pendingName);
      ctx.print(`renamed session to ${ctx.state.pendingName}`);
    } catch (error) {
      ctx.print(`rename failed: ${error.message}`);
    }
    ctx.state.pendingName = null;
  }
}

async function sendMessage(ctx, text) {
  const backend = ctx.backend;
  const sessionId = ctx.state.sessionId;
  const cwd = ctx.state.cwd || process.cwd();
  clearInputLine();
  const requestSessionId = sessionId ?? randomUUID();
  log("info", "cli", `event=user_message session=${requestSessionId} bytes=${text.length}`);
  renderer.user(text);
  renderer.startReply();
  const controller = new AbortController();
  activeAbortController = controller;
  const canSetRaw = input.isTTY && typeof input.setRawMode === "function";
  const wasRaw = canSetRaw ? input.isRaw === true : false;
  if (canSetRaw) {
    input.setRawMode(true);
  }
  try {
    const done = await streamChat(
      backend,
      requestSessionId,
      text,
      ({ event, data }) => {
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
        } else if (event === "compaction") {
          renderer.compaction(data);
        } else if (event === "error") {
          throw new Error(data.error ?? "unknown error");
        }
      },
      cwd,
      controller.signal,
    );
    renderer.endReply();
    log("info", "cli",
        `event=stream_end session=${done?.session_id ?? requestSessionId}`);
    return done?.session_id ?? requestSessionId;
  } catch (error) {
    if (error.name === "AbortError" || controller.signal.aborted) {
      try {
        await cancelChat(backend, requestSessionId);
      } catch (cancelError) {
        log("error", "cli",
            `event=cancel_error session=${requestSessionId} message=${cancelError.message}`);
      }
      renderer.interrupted();
      log("info", "cli", `event=stream_interrupted session=${requestSessionId}`);
      return requestSessionId;
    }
    renderer.error(error.message);
    log("error", "cli", `event=stream_error message=${error.message}`);
    return requestSessionId;
  } finally {
    if (activeAbortController === controller) {
      activeAbortController = null;
    }
    if (canSetRaw) {
      input.setRawMode(wasRaw);
    }
  }
}

function makeSelector(editor) {
  return (sessions) =>
    editor.selectList(
      sessions.map((entry) => ({
        label: sessionEntryLabel(entry),
        description: sessionEntryId(entry),
      })),
      "select a session (↑/↓ move, Enter open, Esc cancel):",
    );
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

  const state = {
    sessionId: options.session,
    cwd: options.cwd ? path.resolve(options.cwd) : process.cwd(),
    shouldExit: false,
    pendingName: null,
  };

  const editor = input.isTTY
    ? new LineEditor({
        input,
        output,
        complete: (prefix) => completeCommands(prefix, commands),
        editorCommand: options.editor,
      })
    : null;

  const ctx = {
    backend,
    state,
    renderer,
    commands,
    print: (line) => console.log(line),
    log,
    clearInputLine,
    clearScreen: () => {
      if (output.isTTY) {
        output.write("\x1b[2J\x1b[3J\x1b[H");
      }
    },
    renderHistory: (backendUrl, sessionId) => renderHistory(backendUrl, sessionId),
    selectSession: editor ? makeSelector(editor) : null,
    confirm: editor
      ? async (message) => {
          const result = await editor.readLine(`${message} (y/N) `);
          return !result.exit && /^y(es)?$/i.test((result.text ?? "").trim());
        }
      : null,
  };

  const sessions = await listSessions(backend).catch(() => []);
  if (sessions.length > 0) {
    console.log(`found ${sessions.length} session(s); use /resume to pick one`);
  }

  // 启动会话解析顺序：--session > --continue > --resume。
  if (!state.sessionId && options.continueSession && sessions.length > 0) {
    state.sessionId = sessionEntryId(sessions[0]);
    console.log(`continuing session ${state.sessionId}`);
  } else if (!state.sessionId && options.resume && sessions.length > 0) {
    if (editor) {
      const index = await ctx.selectSession(sessions);
      if (index !== null && index !== undefined && sessions[index]) {
        state.sessionId = sessionEntryId(sessions[index]);
      }
    } else {
      sessions.forEach((entry, index) => {
        console.log(`  ${index + 1}. ${sessionEntryLabel(entry)}  (${sessionEntryId(entry)})`);
      });
      console.log("use /resume <index|id> to resume one");
    }
  }

  if (options.name) {
    if (state.sessionId) {
      try {
        await renameSession(backend, state.sessionId, options.name);
        console.log(`renamed session to ${options.name}`);
      } catch (error) {
        console.error(`rename failed: ${error.message}`);
      }
    } else {
      state.pendingName = options.name;
    }
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
    if (editor) {
      while (!state.shouldExit) {
        const label = state.sessionId ? state.sessionId.slice(0, 8) : "new";
        const result = await editor.readLine(`[${label}] ❯ `);
        if (result.exit) {
          break;
        }
        await handleLine(ctx, result.text);
      }
    } else {
      const lines = readlinePromises.createInterface({ input, terminal: false });
      for await (const line of lines) {
        await handleLine(ctx, line);
        if (state.shouldExit) {
          break;
        }
      }
      lines.close();
    }
  } finally {
    clearInputLine();
    // readLine/selectList 会 resume stdin 以接收按键；退出主循环后若不暂停，
    // 处于 flowing 状态的 stdin 会让 Node 进程一直存活，表现为 /quit 卡住。
    input.pause?.();
  }
}

main().catch((error) => {
  clearInputLine();
  console.error(error);
  log("error", "cli", `event=cli_fatal message=${error.message}`);
  process.exitCode = 1;
});
