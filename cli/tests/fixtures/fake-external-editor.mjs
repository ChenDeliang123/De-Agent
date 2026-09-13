// 假外部编辑器：供 external-editor.test.mjs 使用。
//
// editInExternalEditor 会用：
//   <command> <capturePath> [--fail|--empty] <promptPath>
// 调用本文件。它先把「读到的文件路径 / 内容 / 目录项 / 目录权限」写入
// capturePath，再按 flag 决定退出码或改写 prompt.md。

import { readFileSync, readdirSync, statSync, writeFileSync } from "node:fs";
import { dirname } from "node:path";

const args = process.argv.slice(2);
const capturePath = args.shift();
const flags = args.filter((arg) => arg.startsWith("--"));
const promptPath = args.find((arg) => !arg.startsWith("--"));

if (!capturePath || !promptPath) {
  console.error("fake-external-editor: usage: <capture> [--fail|--empty] <prompt>");
  process.exit(2);
}

const content = readFileSync(promptPath, "utf-8");
const directory = dirname(promptPath);
const capture = {
  filePath: promptPath,
  content,
  entries: readdirSync(directory),
  directoryMode: statSync(directory).mode & 0o777,
};
writeFileSync(capturePath, JSON.stringify(capture), "utf-8");

if (flags.includes("--fail")) {
  process.exit(1);
}
writeFileSync(promptPath, flags.includes("--empty") ? "" : "edited", "utf-8");
process.exit(0);
