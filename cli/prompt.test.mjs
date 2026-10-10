import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import test from "node:test";

import { askUser, askYesNo } from "./prompt.mjs";

class FakeInput extends EventEmitter {
  constructor(isTTY = true) {
    super();
    this.isTTY = isTTY;
    this.isRaw = false;
    this.paused = false;
  }

  setRawMode(value) {
    this.isRaw = value;
  }

  resume() {
    this.paused = false;
  }

  pause() {
    this.paused = true;
  }
}

function fakeOutput() {
  const writes = [];
  return { isTTY: true, write: (text) => writes.push(text), text: () => writes.join("") };
}

test("askUser uses canonical mode and restores the previous raw state", async () => {
  const input = new FakeInput(true);
  input.isRaw = true; // 模拟流式阶段已开启 raw
  const output = fakeOutput();

  const pending = askUser("approve? ", { input, output });
  assert.equal(input.isRaw, false, "must switch to canonical mode while waiting");
  assert.ok(output.text().includes("approve?"));

  input.emit("data", "yes\n");
  const answer = await pending;
  assert.equal(answer, "yes");
  assert.equal(input.isRaw, true, "must restore the previous raw state");
  assert.equal(input.paused, true, "must pause after reading");
  assert.equal(input.listenerCount("data"), 0, "data listener should be removed");
});

test("askUser strips a trailing carriage return", async () => {
  const input = new FakeInput(true);
  const output = fakeOutput();
  const pending = askUser("q: ", { input, output });
  input.emit("data", "hello\r\n");
  assert.equal(await pending, "hello");
});

test("askYesNo accepts y/yes and rejects others", async () => {
  const input = new FakeInput(true);
  const output = fakeOutput();

  const first = askYesNo("ok? (y/N) ", { input, output });
  input.emit("data", "y\n");
  assert.equal(await first, true);

  const second = askYesNo("ok? (y/N) ", { input, output });
  input.emit("data", "n\n");
  assert.equal(await second, false);
});
