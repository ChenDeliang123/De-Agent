# Skills

本目录是内置技能的安装位置（安装包内为 `<share>/de/skills`，源码树回退到
`skills/`）。目录本身可以保持为空；实际技能按下面的结构放置：

```text
skills/
└── release-notes/
    ├── SKILL.md          # 必需：YAML frontmatter + Markdown 正文
    ├── reference.md      # 可选：按需读取
    └── scripts/          # 可选：由模型显式调用 bash 执行
```

`SKILL.md` 示例：

```markdown
---
name: release-notes
description: Write release notes from git history. Use when asked to summarize commits for a release.
---

# Release notes

1. Read commits since the last tag.
2. Group changes by feat / fix / docs / test.
3. Call out breaking changes first.
```

规则：

- `name` 使用小写字母/数字/连字符，最长 64 字符；缺省时回退为目录名。
- `description` 必填，用于 system prompt 中的技能目录；正文不会预先注入。
- 模型在任务匹配时调用只读工具 `skill(name, file?)` 加载正文或技能目录内资源。
- 默认不跟随符号链接；单个技能正文/资源默认最多 64 KiB。
- 用户级技能放在 `<data_dir>/skills`（默认 `~/.de/skills`）；项目级放在
  `<project-root>/.de/skills`；额外根通过 `[skills] extra_roots` 显式配置。

配置与完整规划见 `docs/planning/agent/Skill加载规划.md`。
