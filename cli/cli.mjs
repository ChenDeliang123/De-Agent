import readlinePromises from "node:readline/promises";
import path from "node:path";
import { randomUUID } from "node:crypto";
import { stdin as input, stdout as output } from "node:process";

import { TranscriptRenderer } from "./src/ui/renderer.mjs";
import { BottomRegion } from "./src/ui/bottom.mjs";
import { log } from "./src/app/logger.mjs";
import {
  ackAgentResults,
  approveChat,
  cancelChat,
  effectivePermission,
  getAgents,
  getProjectPermission,
  listSessions,
  loadHistory,
  renameSession,
  setTrust,
  steerChat,
  streamChat,
} from "./src/app/session.mjs";
import { TurnInputAction, TurnInputBuffer } from "./src/app/turn-input.mjs";
import { LineEditor, renderHotkeys } from "./src/ui/editor.mjs";
import { renderStartupLogo } from "./src/ui/logo.mjs";
import { loadCliConfig, readCliVersion, updateCliUiConfig } from "./src/app/config.mjs";
import { PALETTES, detectColorMode, makeStyler, pickSymbols } from "./src/ui/theme.mjs";
import { visibleWidth, wrapVisible } from "./src/ui/text.mjs";
import { resolveEditorCommand } from "./src/ui/external-editor.mjs";
import {
  commands,
  completeCommands,
  renderHelp,
  restoreSessionCwd,
  runCommand,
} from "./src/app/commands/index.mjs";
import { PROMPT_LABEL, formatStatusLine, gitBranch, shortCwd } from "./src/ui/statusbar.mjs";
import { askUser, askYesNo } from "./src/ui/prompt.mjs";

const defaultBackend = "http://127.0.0.1:18081";
// 与 renderer / editor 共享的 styler，支持运行时换主题。
const uiStyler = makeStyler(detectColorMode(process.env, output));
// 底部区域：输入框（上下两条横线）+ 最底部的信息状态行，由 renderer 与 editor 共享。
// 非 TTY（管道）时禁用，退化为直接写 stdout。
const region = new BottomRegion(output, { enabled: output.isTTY === true });
const renderer = new TranscriptRenderer(output, { styler: uiStyler, region });
let activeAbortController = null;
// 审批提示期间暂停全局 Esc/Ctrl+C 中断，避免与审批输入争抢按键。
let approvalPending = false;
// 后台子 Agent 结果的回合内轮询间隔；回合外还会在每轮输入前与回合结束各补一次。
const BACKGROUND_POLL_INTERVAL_MS = 4000;
// 回合内纠偏（M5c）：回合进行中敲的字进这个单行缓冲，Enter 发到当前回合的收件箱。
// { input: TurnInputBuffer, sessionId, ctx }；回合结束后置回 null。
let turnSteer = null;

// 把纠偏缓冲画进底部区域的输入框槽位（回合内 LineEditor 空闲，该槽位空闲；
// 不占 spinner 的状态行、不占 appendOutput 的 pending 行）。空缓冲收起提示。
function renderSteerPrompt() {
  const turnInput = turnSteer?.input;
  if (!turnInput || turnInput.isEmpty()) {
    region.setEditor(null);
    return;
  }
  const width = region.contentWidth;
  // 超宽时只显示尾部（正在输入的地方），与终端的单行输入观感一致。
  const rows = wrapVisible(`${uiStyler.dim("steer> ")}${turnInput.buffer()}`, width);
  const line = rows[rows.length - 1] ?? "";
  region.setEditor({ lines: [line], cursorRow: 0, cursorCol: visibleWidth(line) });
}

// 回合内纠偏：发送成功/失败都只打印一行，不改回合状态、不抛穿。
async function submitTurnSteer(steer, message) {
  const text = String(message ?? "").trim();
  if (text === "") {
    return;
  }
  try {
    await steerChat(steer.ctx.backend, steer.sessionId, text);
    renderer.steerNotice(text);
  } catch (error) {
    steer.ctx.print(`steer failed: ${error.message}`);
  }
}

if (input.isTTY) {
  input.on("keypress", (text, key) => {
    if (!activeAbortController || approvalPending || !turnSteer) {
      return;
    }
    const result = turnSteer.input.handleKey(text, key);
    if (result.action === TurnInputAction.Abort) {
      activeAbortController.abort();
      activeAbortController = null;
      renderSteerPrompt();
      return;
    }
    if (result.action === TurnInputAction.Submit) {
      void submitTurnSteer(turnSteer, result.text);
      renderSteerPrompt();
      return;
    }
    if (result.action === TurnInputAction.Edit) {
      renderSteerPrompt();
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
    cliConfig: null,
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
    } else if (arg === "--cli-config" && i + 1 < argv.length) {
      options.cliConfig = argv[++i];
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
  --cli-config <f>  CLI settings file (default: conf/cli.ini)
  -h, --help        show this help
`);
  console.log(renderHelp(commands));
  console.log(`\n${renderHotkeys()}\n`);
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
  // 回合结束后补打：本轮新完成的后台任务结果（回合外轮询挂点之一）。
  await pollBackgroundResults(ctx, ctx.state.sessionId);
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
  const requestSessionId = sessionId ?? randomUUID();
  log(
    "info",
    "cli",
    `event=user_message session=${requestSessionId} bytes=${text.length} cwd=${cwd}`,
  );
  renderer.user(text);
  renderer.startReply();
  const controller = new AbortController();
  activeAbortController = controller;
  // 回合内纠偏：新会话首回合时 state.sessionId 还是 null，必须用本次请求 id。
  const steer = { input: new TurnInputBuffer(), sessionId: requestSessionId, ctx };
  turnSteer = steer;
  // 回合内周期轮询后台任务：首回合 state.sessionId 还是 null，必须用本次请求 id。
  // 审批提示期间跳过（region 处于挂起态，补打会踩坏交互提示）。
  // unref：定时器不阻止进程退出；finally 里清理。
  const backgroundTimer = setInterval(() => {
    if (approvalPending) {
      return;
    }
    pollBackgroundResults(ctx, requestSessionId);
  }, BACKGROUND_POLL_INTERVAL_MS);
  backgroundTimer.unref?.();
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
      async ({ event, data }) => {
        if (!data) {
          return;
        }
        // 帧上的 agent_id 标识事件归属：空串 = 根 Agent，非空 = 子 Agent。
        const agentId = data.agent_id ?? "";
        if (event === "content") {
          renderer.content(data.content ?? "", agentId);
        } else if (event === "tool_start") {
          renderer.toolStart(
            data.id ?? "",
            data.name ?? "tool",
            data.arguments ?? "",
            agentId,
          );
        } else if (event === "tool_output") {
          renderer.toolOutput(data.content ?? "", agentId);
        } else if (event === "tool_end") {
          renderer.toolEnd(Boolean(data.ok), data.text ?? "", agentId, data.id ?? "");
        } else if (event === "subagent_start") {
          renderer.subagentStart(data);
        } else if (event === "subagent_result") {
          // 后台任务完成（回合内到达）：生命周期行 + 报告摘要。
          // 已打印就立刻回执，否则回合结束后的轮询会把同一条再补打一次；
          // 帧丢失（写失败）时不会回执，轮询补打即自愈。
          renderer.subagentResult(data);
          if (data?.task_id) {
            ackAgentResults(ctx.backend, [String(data.task_id)]).catch(() => {});
          }
        } else if (event === "subagent_end") {
          renderer.subagentEnd(data);
        } else if (event === "subagent_steer") {
          // 纠偏消息真正投递给子 Agent 时才来这一帧，可能滞后于命令回显。
          renderer.subagentSteer(data);
        } else if (event === "compaction") {
          renderer.compaction(data, agentId);
        } else if (event === "approval_request") {
          await handleApproval(ctx, requestSessionId, data);
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
    clearInterval(backgroundTimer);
    // 回合结束：纠偏缓冲作废，收起残余的 `steer> …` 提示（首个按键前可能没画过）。
    if (turnSteer === steer) {
      turnSteer = null;
      renderSteerPrompt();
    }
    if (activeAbortController === controller) {
      activeAbortController = null;
    }
    if (canSetRaw) {
      input.setRawMode(wasRaw);
    }
  }
}

// 补打后台子 Agent 的结果（回合内轮询 / 回合结束 / 每轮输入前共用）：
// GET agents?session_id → 逐条渲染 pending_results → ack，避免被反复补打。
// 防重入：同一时刻只允许一个轮询；单次失败只打印一行错误，不抛穿主循环。
// sessionId 为空（新会话尚未发出第一条消息）时直接跳过，不打扰后端。
async function pollBackgroundResults(ctx, sessionId) {
  if (!sessionId || ctx.state.backgroundPollInFlight) {
    return;
  }
  ctx.state.backgroundPollInFlight = true;
  try {
    const info = await getAgents(ctx.backend, sessionId);
    const pending = Array.isArray(info?.pending_results) ? info.pending_results : [];
    if (pending.length === 0) {
      return;
    }
    const taskIds = [];
    for (const item of pending) {
      renderer.subagentResult(item);
      if (item?.task_id) {
        taskIds.push(String(item.task_id));
      }
    }
    if (taskIds.length > 0) {
      await ackAgentResults(ctx.backend, taskIds);
    }
  } catch (error) {
    ctx.print(`background results failed: ${error.message}`);
  } finally {
    // 失败也要复位，避免一次网络抖动让轮询永久停摆。
    ctx.state.backgroundPollInFlight = false;
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

function makeSelect(editor) {
  return (items, prompt) => editor.selectList(items, prompt);
}

// 检测当前工作目录的项目级 `.de/` 配置；未信任时提示用户是否信任。
async function maybePromptProjectTrust(ctx) {
  if (typeof ctx.confirm !== "function") {
    return;
  }
  try {
    const info = await getProjectPermission(ctx.backend, ctx.state.cwd ?? process.cwd());
    if (!info.root || !info.has_config || info.trusted) {
      return;
    }
    ctx.print(`project config found: ${info.root}/.de (currently untrusted)`);
    const ok = await ctx.confirm(
      "Trust this project's .de/ config? (untrusted = can only tighten permissions)",
    );
    await setTrust(ctx.backend, info.root, Boolean(ok));
    ctx.print(ok ? "project trusted" : "project kept untrusted (tighten-only)");
    ctx.log(
      "info",
      "cli",
      `event=project_trust path=${info.root} trusted=${Boolean(ok)}`,
    );
  } catch (error) {
    ctx.log("warning", "cli", `event=project_trust_check_failed message=${error.message}`);
  }
}

// 处理 approval_request：弹交互提示并把决定回传后端。
async function handleApproval(ctx, sessionId, data) {
  const toolCallId = data.id ?? "";
  renderer.approvalRequest(data);
  let decision = "deny";
  approvalPending = true;
  try {
    if (input.isTTY) {
      // 审批提示走 canonical 模式，需要先把底部区域让出来（擦除输入框/状态行）。
      region.suspend();
      let answer;
      try {
        answer = await askUser("[y]es once / [a]lways this session / [N]o: ");
      } finally {
        region.resume();
      }
      const text = String(answer ?? "").trim().toLowerCase();
      if (text === "y" || text === "yes") {
        decision = "allow_once";
      } else if (text === "a" || text === "always") {
        decision = "allow_session";
      }
    }
  } finally {
    approvalPending = false;
  }
  try {
    await approveChat(ctx.backend, sessionId, toolCallId, decision);
    log(
      "info",
      "cli",
      `event=approval_decision session=${sessionId} tool_call=${toolCallId} decision=${decision}`,
    );
  } catch (error) {
    log(
      "error",
      "cli",
      `event=approval_error session=${sessionId} message=${error.message}`,
    );
  }
  renderer.approvalResolved(decision);
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  if (options.help) {
    printHelp();
    return;
  }

  const backend = options.backend.replace(/\/+$/, "");
  log("info", "cli", `event=cli_start backend=${backend}`);

  const state = {
    sessionId: options.session,
    cwd: options.cwd ? path.resolve(options.cwd) : process.cwd(),
    cwdExplicit: options.cwd != null,
    shouldExit: false,
    pendingName: null,
    mode: null,
    branch: null,
  };

  const cliConfig = loadCliConfig({ explicit: options.cliConfig });
  const initialTheme =
    typeof cliConfig.theme === "string" && PALETTES[cliConfig.theme]
      ? cliConfig.theme
      : "dark";
  uiStyler.setPalette(PALETTES[initialTheme]);

  // 启动欢迎块（logo + 元信息 + 快捷键提示）：只在交互终端打印。
  // 放在 BottomRegion 首次渲染（refreshPromptState → setFooter）之前，
  // 此时区域尚未绘制，直接写 stdout 不会踩到底部状态行。
  if (output.isTTY === true) {
    const version = readCliVersion();
    output.write(
      renderStartupLogo(uiStyler, {
        columns: output.columns ?? 80,
        titleLines: version ? ["de agent", `v${version}`] : ["de agent"],
        info: [
          `backend=${backend.replace(/^https?:\/\//, "")}`,
          shortCwd(state.cwd),
        ],
        hint: "/help 查看命令 · Esc 中断当前回复 · Ctrl+D 退出",
      }),
    );
  }

  const symbolEnv = { ...process.env };
  if (cliConfig.nerdFont) {
    symbolEnv.DE_NERD_FONT = "1";
  }
  const symbols = pickSymbols(symbolEnv);
  renderer.setSymbols(symbols);
  state.theme = initialTheme;
  state.cliConfigPath = cliConfig.path;
  const editorCommand = resolveEditorCommand({
    flag: options.editor,
    configured: cliConfig.externalEditor,
  });

  // 输入框渲染依赖底部区域（需要 stdout 是 TTY）；否则退回逐行读取，避免
  // 往重定向的文件里写转义序列。
  const editor = input.isTTY && output.isTTY
    ? new LineEditor({
        input,
        output,
        complete: (prefix) => completeCommands(prefix, commands),
        editorCommand,
        styler: uiStyler,
        region,
        symbols,
      })
    : null;

  // 最底部状态行：`~/proj (main) · confirm · f920924d`。
  // 字段缺失自动省略；超宽时从右往左丢字段，保证关键信息不被截半。
  const updateFooter = () => {
    region.setFooter(
      formatStatusLine(
        {
          cwd: state.cwd,
          branch: state.branch,
          mode: state.mode,
          sessionId: state.sessionId,
        },
        output.columns,
        uiStyler,
      ),
    );
  };

  // 启动期提示同样走底部区域：状态行已绘制，直接 console.log 会写进状态行那一行。
  const emit = (line) => region.appendOutput(`${line}\n`);

  const ctx = {
    backend,
    state,
    renderer,
    editor,
    commands,
    // 命令输出同样经过底部区域，保证最底部的状态行始终可见。
    print: (line) => region.appendOutput(`${line}\n`),
    log,
    clearScreen: () => {
      if (output.isTTY) {
        region.reset();
        output.write("\x1b[2J\x1b[3J\x1b[H");
        region.render();
      }
    },
    updateFooter,
    renderHistory: (backendUrl, sessionId) => renderHistory(backendUrl, sessionId),
    selectSession: editor ? makeSelector(editor) : null,
    select: editor ? makeSelect(editor) : null,
    confirm: input.isTTY
      ? async (message) => {
          region.suspend();
          try {
            return await askYesNo(`${message} (y/N) `, { input, output });
          } finally {
            region.resume();
          }
        }
      : null,
  };
  ctx.promptProjectTrust = () => maybePromptProjectTrust(ctx);

  // 运行时换主题并持久化到 [ui] theme。
  ctx.setTheme = (name) => {
    if (!PALETTES[name]) {
      return false;
    }
    uiStyler.setPalette(PALETTES[name]);
    state.theme = name;
    if (state.cliConfigPath) {
      try {
        updateCliUiConfig(state.cliConfigPath, { theme: name });
      } catch (error) {
        log("warning", "cli", `event=theme_persist_failed message=${error.message}`);
      }
    }
    return true;
  };

  // 输入提示符状态：cwd 分支 + 当前权限模式（best-effort）。
  ctx.refreshPromptState = async () => {
    state.branch = gitBranch(state.cwd);
    renderer.setCwd(state.cwd);
    try {
      const info = await effectivePermission(backend, state.cwd);
      state.mode = info.mode ?? null;
    } catch {
      state.mode = null;
    }
    // 权限模式/分支变化后立即刷新最底部状态行。
    updateFooter();
  };
  await ctx.refreshPromptState();

  const sessions = await listSessions(backend).catch(() => []);
  if (sessions.length > 0) {
    emit(`found ${sessions.length} session(s); use /resume to pick one`);
  }

  // 启动会话解析顺序：--session > --continue > --resume。
  if (!state.sessionId && options.continueSession && sessions.length > 0) {
    state.sessionId = sessionEntryId(sessions[0]);
    emit(`continuing session ${state.sessionId}`);
  } else if (!state.sessionId && options.resume && sessions.length > 0) {
    if (editor) {
      const index = await ctx.selectSession(sessions);
      if (index !== null && index !== undefined && sessions[index]) {
        state.sessionId = sessionEntryId(sessions[index]);
      }
    } else {
      sessions.forEach((entry, index) => {
        emit(`  ${index + 1}. ${sessionEntryLabel(entry)}  (${sessionEntryId(entry)})`);
      });
      emit("use /resume <index|id> to resume one");
    }
  }

  if (options.name) {
    if (state.sessionId) {
      try {
        await renameSession(backend, state.sessionId, options.name);
        emit(`renamed session to ${options.name}`);
      } catch (error) {
        emit(`rename failed: ${error.message}`);
      }
    } else {
      state.pendingName = options.name;
    }
  }

  const restoredCwd = await restoreSessionCwd(ctx);
  if (restoredCwd) {
    emit(`restored cwd ${restoredCwd}`);
  }

  await ctx.promptProjectTrust();

  if (state.sessionId) {
    try {
      await renderHistory(backend, state.sessionId);
    } catch (error) {
      emit(`cannot load session ${state.sessionId}: ${error.message}`);
      state.sessionId = null;
    }
  }

  try {
    if (editor) {
      while (!state.shouldExit) {
        // 每轮输入前刷新状态行：会话 id / 权限模式 / cwd 可能已被上一条命令改掉。
        updateFooter();
        // 下一次渲染前，先把后台子 Agent 的迟到结果补打出来。
        await pollBackgroundResults(ctx, state.sessionId);
        const result = await editor.readLine(PROMPT_LABEL);
        if (result.exit) {
          break;
        }
        await handleLine(ctx, result.text);
      }
    } else {
      const lines = readlinePromises.createInterface({ input, terminal: false });
      for await (const line of lines) {
        await pollBackgroundResults(ctx, state.sessionId);
        await handleLine(ctx, line);
        if (state.shouldExit) {
          break;
        }
      }
      lines.close();
    }
  } finally {
    region.erase();
    // readLine/selectList 会 resume stdin 以接收按键；退出主循环后若不暂停，
    // 处于 flowing 状态的 stdin 会让 Node 进程一直存活，表现为 /quit 卡住。
    input.pause?.();
  }
}

main().catch((error) => {
  region.erase();
  console.error(error);
  log("error", "cli", `event=cli_fatal message=${error.message}`);
  process.exitCode = 1;
});
