// Markdown-lite：按行渲染助手输出（零依赖）。
//
// 只覆盖 coding agent 常用的少量语法：标题、列表、引用、分隔线、代码块、
// 行内代码与加粗。流式场景下由 renderer 按“完整行”喂进来，代码块状态跨行保持。

function renderInline(text, t) {
  let out = "";
  const parts = String(text).split(/(`[^`]*`)/g);
  for (const part of parts) {
    if (part.length >= 2 && part.startsWith("`") && part.endsWith("`")) {
      out += t.fg("mdCode", part.slice(1, -1));
    } else {
      out += part.replace(/\*\*([^*]+)\*\*/g, (_match, bold) => t.bold(bold));
    }
  }
  return out;
}

export function createMarkdownRenderer(styler) {
  const t = styler;
  let inCode = false;

  return {
    reset() {
      inCode = false;
    },

    // 渲染一个完整行（不含换行），返回若干样式化后的行（不含外层缩进）。
    renderLine(line) {
      const raw = String(line);

      const fence = raw.match(/^\s*```(.*)$/);
      if (fence) {
        if (!inCode) {
          inCode = true;
          const lang = fence[1].trim();
          return [t.dim(`╭─ ${lang || "code"}`)];
        }
        inCode = false;
        return [t.dim("╰─")];
      }

      if (inCode) {
        return [raw.length ? `${t.fg("borderMuted", "│")} ${t.fg("mdCode", raw)}` : ""];
      }

      const heading = raw.match(/^(#{1,6})\s+(.*)$/);
      if (heading) {
        return [t.fg("mdHeading", t.bold(renderInline(heading[2], t)))];
      }

      if (/^\s*(?:-{3,}|\*{3,}|_{3,})\s*$/.test(raw)) {
        return [t.dim("─".repeat(40))];
      }

      const quote = raw.match(/^>\s?(.*)$/);
      if (quote) {
        return [`${t.dim("│")} ${t.dim(renderInline(quote[1], t))}`];
      }

      const bullet = raw.match(/^(\s*)[-*+]\s+(.*)$/);
      if (bullet) {
        return [`${bullet[1]}${t.fg("mdListBullet", "•")} ${renderInline(bullet[2], t)}`];
      }

      const ordered = raw.match(/^(\s*)(\d+)\.\s+(.*)$/);
      if (ordered) {
        return [
          `${ordered[1]}${t.fg("mdListBullet", `${ordered[2]}.`)} ${renderInline(ordered[3], t)}`,
        ];
      }

      return [renderInline(raw, t)];
    },
  };
}
