import assert from "node:assert/strict";
import test from "node:test";

import {
  commands,
  completeCommands,
  findCommand,
  renderHelp,
  runCommand,
  suggestCommand,
} from "./commands.mjs";

function makeContext(overrides = {}) {
  const printed = [];
  const state = { sessionId: null, shouldExit: false, ...(overrides.state ?? {}) };
  const ctx = {
    backend: "http://127.0.0.1:1",
    state,
    commands,
    print: (line) => printed.push(line),
    log: () => {},
    clearInputLine: () => {},
    clearScreen: () => printed.push("<clear>"),
    renderHistory: async () => printed.push("<history>"),
    ...overrides,
  };
  return { ctx, printed, state };
}

test("registry exposes the P0 commands", () => {
  const names = commands.map((command) => command.name);
  for (const expected of [
    "new",
    "sessions",
    "open",
    "compact",
    "help",
    "hotkeys",
    "clear",
    "quit",
  ]) {
    assert.ok(names.includes(expected), `missing command /${expected}`);
  }
});

test("findCommand resolves primary names and aliases", () => {
  assert.equal(findCommand("help").name, "help");
  assert.equal(findCommand("exit").name, "quit");
  assert.equal(findCommand("HELP").name, "help");
  assert.equal(findCommand("nope"), null);
});

test("completeCommands filters by prefix", () => {
  assert.deepEqual(
    completeCommands("he").map((command) => command.name),
    ["help"],
  );
  assert.deepEqual(
    completeCommands("h").map((command) => command.name),
    ["help", "hotkeys"],
  );
  assert.deepEqual(
    completeCommands("").map((command) => command.name),
    commands.map((command) => command.name),
  );
  assert.deepEqual(completeCommands("zzz"), []);
});

test("renderHelp is generated from the registry", () => {
  const help = renderHelp();
  for (const command of commands) {
    assert.ok(help.includes(`/${command.name}`), `help missing /${command.name}`);
  }
  assert.ok(help.includes("Session:"));
  assert.ok(help.includes("Local:"));
});

test("suggestCommand proposes the closest name", () => {
  assert.equal(suggestCommand("hepl"), "help");
  assert.equal(suggestCommand("quitt"), "quit");
  assert.equal(suggestCommand("zzzzzz"), null);
});

test("runCommand executes a known command and reports unknown ones", async () => {
  const { ctx, printed, state } = makeContext();

  assert.equal(await runCommand(ctx, "/new"), true);
  assert.deepEqual(printed, ["new session"]);
  assert.equal(state.sessionId, null);

  printed.length = 0;
  await runCommand(ctx, "/hepl");
  assert.match(printed[0], /unknown command: \/hepl; did you mean \/help\?/);

  printed.length = 0;
  await runCommand(ctx, "/zzzzzz");
  assert.match(printed[0], /unknown command: \/zzzzzz; use \/help/);
});

test("runCommand passes the argument after the command name", async () => {
  let received = null;
  const custom = [
    {
      name: "echo",
      aliases: [],
      description: "echo",
      category: "local",
      handler: async (_ctx, arg) => {
        received = arg;
      },
    },
  ];
  const { ctx } = makeContext({ commands: custom });
  await runCommand(ctx, "/echo hello world");
  assert.equal(received, "hello world");
});

test("clear and quit commands drive the injected context", async () => {
  const { ctx, printed, state } = makeContext();
  await runCommand(ctx, "/clear");
  assert.deepEqual(printed, ["<clear>"]);

  await runCommand(ctx, "/quit");
  assert.equal(state.shouldExit, true);
});
