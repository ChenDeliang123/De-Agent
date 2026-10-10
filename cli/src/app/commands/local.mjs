// 本地界面类指令（category=local）：/theme /clear /quit。
// 各自单独导出，便于 index.mjs 按历史顺序拼装注册表。

export const themeCommand = {
  name: "theme",
  aliases: [],
  description: "Show or change the color theme",
  argHint: "[dark|light]",
  category: "local",
  handler: async (ctx, arg) => {
    const available = ["dark", "light"];
    const token = (arg ?? "").trim().toLowerCase();
    if (!token) {
      ctx.print(`theme: ${ctx.state.theme ?? "dark"}`);
      ctx.print(`available: ${available.join(", ")}`);
      return;
    }
    if (!available.includes(token)) {
      ctx.print(`unknown theme: ${token}; available: ${available.join(", ")}`);
      return;
    }
    if (typeof ctx.setTheme === "function" && ctx.setTheme(token)) {
      ctx.print(`theme: ${token}`);
    } else {
      ctx.print("theme change failed");
    }
  },
};

export const clearCommand = {
  name: "clear",
  aliases: [],
  description: "Clear the screen",
  argHint: "",
  category: "local",
  handler: async (ctx) => {
    ctx.clearScreen();
  },
};

export const quitCommand = {
  name: "quit",
  aliases: ["exit"],
  description: "Exit",
  argHint: "",
  category: "local",
  handler: async (ctx) => {
    ctx.state.shouldExit = true;
  },
};
