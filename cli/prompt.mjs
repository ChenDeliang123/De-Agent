// 一次性单行输入（canonical 模式，供审批/确认提示使用）。
//
// 为什么不用 readline.createInterface？
//   主输入行的 LineEditor 已经对同一个 stdin 调过 readline.emitKeypressEvents()，
//   若再 createInterface 会对同一个流重复安装 keypress 解析，导致“一次按键触发
//   多次”——表现为字符被重复插入、刷屏（如 1111...）。
//
// 因此这里走最稳的路径：等待回车时把 stdin 切到 canonical 模式（setRawMode(false)），
// 让 TTY 自己做回显与行编辑，用 'data' 事件按行读取；不手动重绘、不进入 raw 模式。
//
// 约定：
//   - 进入时记录原 raw 状态，退出时恢复（流式阶段原状态是 true，会被还原）；
//   - 读取完成后移除监听并 pause，避免监听泄漏。

export function askUser(question, { input = process.stdin, output = process.stdout } = {}) {
  output.write(question);

  const canRaw = typeof input.setRawMode === "function";
  const previousRaw = canRaw ? input.isRaw === true : false;
  // 关键：等待用户按回车时必须是 canonical（非 raw）模式。
  if (canRaw) {
    input.setRawMode(false);
  }

  return new Promise((resolve) => {
    let buffer = "";

    const finish = (value) => {
      input.removeListener("data", onData);
      input.removeListener("end", onEnd);
      input.pause?.();
      if (canRaw) {
        input.setRawMode(previousRaw);
      }
      output.write("\n");
      resolve(value);
    };

    const onData = (chunk) => {
      buffer += chunk.toString("utf8");
      const newline = buffer.indexOf("\n");
      if (newline === -1) {
        return;
      }
      finish(buffer.slice(0, newline).replace(/\r$/, ""));
    };

    const onEnd = () => finish(buffer.replace(/\r?\n$/, ""));

    input.on("data", onData);
    input.once("end", onEnd);
    input.resume?.();
  });
}

// 便捷封装：读取一行并判断是否为肯定回答（y / yes）。
export async function askYesNo(question, options) {
  const answer = await askUser(question, options);
  return /^y(es)?$/i.test(String(answer).trim());
}
