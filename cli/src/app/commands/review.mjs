// 审核类指令（category=review）：/review prepare|cancel|setup|help。
//
// 两段式：编码会话用 /review prepare 生成 pack，然后在新终端执行
// `de review <pack>` 启动独立 reviewer 会话；本指令不切换当前会话角色。
import { cancelChat, reviewConfig } from "../session.mjs";
import {
  formatReviewResult,
  parseReviewPrepareArgs,
  reviewPrepare,
  reviewPrepareHelp,
} from "../review-prepare.mjs";

function tokenize(text) {
  // 简单 shell-like 分词：支持单/双引号，够用于 /review prepare 参数。
  const tokens = [];
  let current = "";
  let quote = null;
  for (let i = 0; i < text.length; i += 1) {
    const ch = text[i];
    if (quote) {
      if (ch === quote) {
        quote = null;
      } else {
        current += ch;
      }
    } else if (ch === '"' || ch === "'") {
      quote = ch;
    } else if (/\s/.test(ch)) {
      if (current) {
        tokens.push(current);
        current = "";
      }
    } else {
      current += ch;
    }
  }
  if (current) {
    tokens.push(current);
  }
  return tokens;
}

async function prepare(ctx, arg) {
  const tokens = tokenize(arg);
  const options = parseReviewPrepareArgs(tokens);
  if (options.help) {
    ctx.print(reviewPrepareHelp());
    return;
  }
  if (!options.feature) {
    ctx.print("usage: /review prepare <feature> [--base <ref>] [--context <path>]...");
    return;
  }
  try {
    const config = await reviewConfig(ctx.backend);
    if (!config.enabled) {
      ctx.print("review is disabled ([review] enabled=false)");
      return;
    }
    const result = await reviewPrepare({
      ...options,
      backend: ctx.backend,
      cwd: ctx.state.cwd || options.cwd || process.cwd(),
      producer: "de-session",
      sessionId: ctx.state.sessionId ?? undefined,
    });
    ctx.print(formatReviewResult(result));
    ctx.log(
      "info",
      "cli",
      `event=review_prepare session=${ctx.state.sessionId ?? "-"} pack=${result.pack_path ?? ""}`,
    );
  } catch (error) {
    ctx.print(`review prepare failed: ${error.message}`);
    ctx.log("error", "cli", `event=review_prepare_failed message=${error.message}`);
  }
}

export const reviewCommands = [
  {
    name: "review",
    aliases: [],
    description: "Prepare or cancel a code review pack",
    argHint: "prepare|cancel|setup",
    category: "review",
    handler: async (ctx, arg) => {
      const [subcommand, ...rest] = tokenize(arg);
      if (!subcommand || subcommand === "help") {
        ctx.print(reviewPrepareHelp());
        ctx.print("");
        ctx.print("/review cancel    stop the current review turn (draft report is kept)");
        ctx.print("/review setup     review_setup: detect/approve project build/test argv");
        return;
      }
      if (subcommand === "prepare") {
        await prepare(ctx, rest.join(" "));
        return;
      }
      if (subcommand === "cancel") {
        if (!ctx.state.sessionId) {
          ctx.print("no active session to cancel");
          return;
        }
        try {
          await cancelChat(ctx.backend, ctx.state.sessionId);
          ctx.print("review turn cancelled; draft report (if written) is kept");
        } catch (error) {
          ctx.print(`review cancel failed: ${error.message}`);
        }
        return;
      }
      if (subcommand === "setup") {
        ctx.print(
          "project verification is configured by the reviewer via review_setup(detect/show/save).",
        );
        ctx.print("start it with: de review <pack>");
        return;
      }
      ctx.print(`unknown /review subcommand: ${subcommand}`);
    },
  },
];
