# de agent

**de agent** 是一个通用型 coding agent（作者：**德先生**）：基于 C++ 实现，通过
DeepSeek 进行对话、按需调用 MCP 与内置文件/命令工具，并以 A2A
（Agent-to-Agent）协议对外提供服务，配套一个零 npm 依赖的 Node.js 终端
客户端（`cli/`）。

它面向任意语言、任意仓库的软件开发，而不是绑定某个特定项目或模型。

## 特性

- 基于 C++20 实现 Agent Runtime，复用 [a2a-protocol] 作为 A2A v1.0 协议库。
- 接入 DeepSeek Chat Completions：
  - 同步对话
  - SSE 逐 token 流式输出
  - `tools` / `tool_calls` 支持
- 接入 [mcp-extension-platform]：
  - 自动发现并加载 MCP 工具
  - 模型发起 `tool_calls` 后自动执行并回填结果
- 任务与消息历史通过 SQLite 持久化。
- 上下文压缩：超阈值时自动摘要较早消息，避免长会话撞上下文窗口。
- 文件工具路径沙箱：`read`/`write`/`edit` 默认只能访问工作目录及其子目录。
- 权限模式：按工作路径选择 `confirm` / `workspace` / `full`，后端据此裁决工具调用。
- 内置本地 REST + SSE 后端，配套零 npm 依赖的终端 coding 客户端。
- 代码审核 Agent：`de review` 打开独立 `reviewer` 会话；编码方先生成 review pack
  （diff/untracked/digest/意图/验证声明），reviewer 只读源码、按声明 argv 复跑
  构建/测试，并把报告写到 `docs/review/`。

[a2a-protocol]: https://gitee.com/chen_dl/a2a-protocol
[mcp-extension-platform]: https://gitee.com/chen_dl/mcp-extension-platform

## 系统架构

```text
┌──────────────┐      ┌──────────────────────┐
│  Terminal    │ ───▶ │  Local HTTP Backend  │
│  CLI (node)  │      │   C++ REST + SSE     │
└──────────────┘      └──────────┬───────────┘
                                 │
┌──────────────┐                 │
│  A2A Client  │ ────────────────┤
└──────────────┘                 ▼
                    ┌──────────────────────────┐
                    │      AgentRuntime         │
                    │  消息历史 / 工具调用循环   │
                    └──────┬──────────┬─────────┘
                           │          │
              ┌────────────▼────┐  ┌──▼────────────┐
              │    DeepSeek     │  │  MCP Server   │
              │ Chat Completions│  │ stdio JSON-RPC│
              └─────────────────┘  └───────────────┘

              SQLite TaskStore
              （任务与会话历史）
```

核心链路：

```text
用户输入
  → AgentRuntime
  → DeepSeek 流式生成
  → 若模型请求 tool_calls
  → 调用 MCP 工具并回填
  → 继续生成，直到输出最终回答
  → 写入 SQLite
```

## 目录结构

```text
.
├── CMakeLists.txt              # 顶层 C++ 构建
├── conf/
│   ├── app.ini.example         # 公开配置模板
│   └── conf.ini                # 本地密钥配置（gitignore）
├── include/aiagent/
│   ├── agent/                  # Agent Runtime
│   ├── llm/                    # DeepSeek Client
│   ├── mcp/                    # MCP stdio 客户端桥
│   ├── tools/                  # 工具抽象接口
│   └── web/                    # 本地 HTTP 后端（终端 CLI 使用）
├── src/                        # 对应实现
├── tests/                      # C++ 测试
├── examples/                   # CLI 示例
├── cli/                        # 终端 coding 客户端（Node，零依赖）
│   ├── cli.mjs                 # 入口（de 启动器调用）
│   ├── src/ui/                 # 呈现层：输入框/底部状态行/渲染/markdown/主题
│   ├── src/app/                # 应用层：指令注册表、会话 API、配置、日志
│   └── tests/                  # Node 测试（node --test）
├── de                          # 一键启动器（守护后端 + CLI）
├── scripts/                    # install/uninstall de
├── vendor/
│   ├── a2a-protocol/           # Git submodule
│   └── mcp-extension-platform/ # Git submodule
└── third_party/
```

## 环境要求

- Linux，或 Windows 10 1809+ / Windows 11 x64
- CMake ≥ 3.21
- 支持 C++20 的编译器（GCC 11+ / Clang 14+ / MSVC v143+；Windows 本机基线为 MinGW-w64）
- libcurl 开发库
- Node.js ≥ 18（运行终端 CLI 时）
- Python 3（可选，MCP Python 插件需要）

CMake 配置阶段会自动获取 `cpp-httplib` 与 SQLite，需要可用的网络连接。

## 安装（绿色包）

从源码构建并打出一个解包即用的绿色版：

```bash
git submodule update --init --recursive
scripts/package-portable.sh
# → dist/v<version>/de-agent-<version>-linux-<arch>.tar.gz（附 .sha256）
```

在目标机器上：

```bash
tar xzf de-agent-<version>-linux-<arch>.tar.gz
cd de-agent-<version>-linux-<arch>
./de init      # 在 ~/.de/conf/conf.ini 生成配置，填入 DeepSeek api_key
./de doctor    # 检查后端/Node/配置/端口
./de           # 启动后端并进入终端客户端
```

运行依赖：`libcurl.so.4`（后端动态链接）、`Node.js ≥ 18`（终端客户端）、
`curl` 命令（`de` 健康检查）。数据与日志默认放在
`${DE_HOME:-$XDG_DATA_HOME/de:-$HOME/.de}`，与安装目录分离，升级包不影响会话。

`de` 会自动识别布局：**源码树**（`<repo>/de`）继续使用仓库内的 `.runtime` 与
`build/db`，开发体验不变；**安装包 / 系统安装**（`<prefix>/bin/de`）使用用户数据
目录。可用 `de doctor` 查看当前布局解析出的全部路径。

### 可选组件：MCP 工具

主包**默认不含 MCP**（保持约 2 MB、无 Python 依赖）。MCP Server 是独立子项目，
以可选包形式单独分发；想要时：

```bash
# 1) 先构建 MCP Server
cmake -S vendor/mcp-extension-platform \
  -B vendor/mcp-extension-platform/build -DCMAKE_BUILD_TYPE=Release
cmake --build vendor/mcp-extension-platform/build -j

# 2) 出主包 + MCP 可选包
scripts/package-portable.sh --with-mcp
# → dist/v<version>/de-agent-...tar.gz          （约 2 MB）
# → dist/v<version>/de-agent-mcp-...tar.gz      （约 10 MB，只含 share/de/mcp/）
```

把可选包解到**主包根目录**即生效（不必重新打主包）：

```bash
tar xzf de-agent-mcp-<version>-linux-<arch>.tar.gz \
    -C de-agent-<version>-linux-<arch>/
./de restart        # 重启后端以加载 MCP
./de doctor         # 确认路径
```

生效标志：`GET /api/v1/tools` 会多出 MCP 工具；`de logs` 里可见
`[mcp] event=server_start ... binary=<prefix>/share/de/mcp/mcp_server`。
后端会自动从 `<prefix>/share/de/mcp/{mcp_server,plugins}` 发现，无需改配置。

> Python 插件需要目标机器有 `python3`；`native` / `shell` 等插件不需要。
> 也可在你的 `conf/conf.ini` 的 `[mcp]` 段显式指定 `binary` / `plugins` 覆盖默认发现。

## Windows（MinGW-w64 + PowerShell 启动器）

Windows 使用 `de.cmd`（三行 shim）+ `de.ps1`，命令与 POSIX `de` 对齐：

```powershell
# 1) 构建（MinGW + Ninja + 静态 libcurl，见 docs/planning/platform/Windows适配规划.md P0）
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-windows.ps1

# 2) 初始化 / 检查 / 启动
.\de.cmd init      # 源码树写入 conf/conf.ini；安装态写入 %USERPROFILE%\.de\conf
.\de.cmd doctor
.\de.cmd           # 启动后端并进入 CLI
.\de.cmd status
.\de.cmd stop
.\de.cmd logs backend
.\de.cmd logs cli
```

Windows 命令：

| 命令 | 作用 |
| --- | --- |
| `de.cmd init` | 复制 `app.ini.example`/`cli.ini.example`，不覆盖已有文件 |
| `de.cmd doctor` | 检查后端、CLI、Node、curl、数据目录、端口 |
| `de.cmd [options]` | 启动后端（若未运行）并进入终端 CLI |
| `de.cmd stop` / `status` / `restart` | 进程管理（PID 文件在 runtime 目录） |
| `de.cmd logs [backend\|cli]` | `Get-Content -Wait` 跟踪日志 |
| `de.cmd purge [--yes]` | 删除用户数据目录 |
| `de.cmd --no-backend -- ...` | 只运行 CLI（参数透传） |

布局自动识别：**源码树**（`<repo>\de.cmd`，后端 `build-mingw\aiagent_app.exe`，
数据/日志在仓库 `.runtime` 与 `build-mingw\db`）、**绿色包**（`<pkg>\de.cmd` +
`bin\aiagent_app.exe`）、**安装态**（`<prefix>\bin\de.cmd`）。安装态数据目录为
`%USERPROFILE%\.de`（`%LOCALAPPDATA%\de` 兜底）。可用 `de.cmd doctor` 查看解析结果。

可选 MCP：构建到根目录 `build-mcp\` 后，源码树启动会自动发现
`build-mcp\mcp_server\mcp_server.exe`；安装包则放在
`<prefix>\share\de\mcp\{mcp_server.exe,plugins\}`。

### Windows 本地回归

一条命令覆盖 a2a、根项目、CLI、MCP（加 `-Package` 时再加 ZIP 打包）：

```powershell
# 日常回归
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test-windows.ps1

# Release 配置
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test-windows.ps1 -Configuration Release

# 最终验收：额外构建并校验 Windows ZIP/MCP ZIP
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test-windows.ps1 -Package
```

前置与 `build-windows.ps1` 相同；a2a 测试还需要缓存
`<AIAGENT_DEPS_DIR>\googletest-1.15.2`。脚本分段执行，某段失败不会掩盖后续段；
日志在 `build-mingw\smoke\{a2a,root,cli,mcp,package}.log`。

预期显式 `SKIP`：

- 无创建符号链接权限时，根项目 4 个 symlink 用例跳过；
- 未配置 DashScope key 时，`test_rag_embedding.py` 跳过；
- `test_rag_integration.py` 在 Windows 因依赖 bash 管道跳过。

`scripts/test-windows.ps1` 会把 `SKIP:` 行汇总到终端；除这些明确项外，平台差异
不允许静默跳过。Linux 回归在 Ubuntu 虚拟机执行，命令见
`docs/planning/platform/Windows适配规划.md` 的 P7 段。

### Windows 绿色 ZIP

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/package-windows.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/package-windows.ps1 -WithMcp
# → dist/v<version>/de-agent-<version>-windows-x64.zip
# → dist/v<version>/de-agent-mcp-...zip          （-WithMcp）
# → dist/v<version>/SHA256SUMS、RELEASE.md
```

主包解压后直接运行 `bin` 旁的 `de.cmd`：

```powershell
Expand-Archive de-agent-<version>-windows-x64.zip -DestinationPath .
cd de-agent-<version>-windows-x64
.\de.cmd init
notepad "$env:USERPROFILE\.de\conf\conf.ini"   # 或按 de.cmd doctor 打印的路径
.\de.cmd doctor
.\de.cmd
```

MCP 可选包根为 `share/de/mcp/`，解到主包根即可：

```powershell
Expand-Archive de-agent-mcp-<version>-windows-x64.zip -DestinationPath .
.\de.cmd restart     # 重新加载 MCP
```

ZIP 内的 EXE/DLL 已静态链接 MinGW 运行时（不依赖 libstdc++/libgcc/winpthread DLL），
目标机只需 Windows 10 1809+ 与 Node.js。校验用 PowerShell：

```powershell
Get-FileHash .\de-agent-<version>-windows-x64.zip -Algorithm SHA256
```

> Git/MSYS 的 `sha256sum -c` 在 Windows 上对大文件存在误报，`SHA256SUMS` 以
> PowerShell `Get-FileHash` 校验为准。

已知限制（v1）：

- 默认 shell 是 `cmd.exe /d /s /c`，不是 bash；模型提示词/工具描述已按平台切换。
- 推荐 Windows Terminal；经典 conhost 的颜色/光标/宽字符能力有限。
- `system_monitor` Python 插件在 Windows 明确返回 unsupported（不要依赖 `/proc`）。
- ARM64 暂不支持；长路径/UNC 的企业环境需后续硬化。

### 系统包（deb / rpm）

想交给系统包管理器安装（自动带依赖、可卸载）：

```bash
scripts/package-deb-rpm.sh
# → dist/v<version>/de-agent_<version>_amd64.deb
# → dist/v<version>/de-agent-<version>-<release>.<arch>.rpm   （需构建机装 rpm-build）
```

安装 / 卸载：

```bash
# Debian / Ubuntu
sudo apt install ./de-agent_0.1.3_amd64.deb   # 自动带 libcurl4 / nodejs(>=18) / curl
sudo apt remove  de-agent

# RHEL / Fedora / openSUSE
sudo dnf install ./de-agent-0.1.3-1.x86_64.rpm
sudo dnf remove  de-agent
```

安装后：可执行文件在 `/usr/bin/de` 与 `/usr/bin/aiagent_app`，脚本资源在
`/usr/lib/de/cli`，配置模板在 `/usr/share/de/conf`；**数据仍在 `~/.de`**。
首次使用 `de init` 生成配置，然后直接 `de`。

- 依赖：`Depends` 由 `dpkg-shlibdeps` 自动分析（`libcurl4`、`libc6`、`libstdc++6`…）；
  `Recommends: nodejs (>= 18), curl, python3`（apt 默认会装；用 nvm 装 node 或发行版
  自带 node 过旧时不会因硬依赖而拒绝安装）。
- 出 rpm 需构建机有 `rpm-build`（Debian/Ubuntu：`sudo apt install rpm`）。
- **发布不需要服务器**：把 `.deb`/`.rpm` 传到 Gitee/GitHub Releases 即可，
  用户 `apt install ./xxx.deb` 下载安装；想要 `apt upgrade` 自动升级再把它们
  放进一个静态 APT/YUM 源（Gitee Pages 等免费静态托管即可）。
- **卸载与清理**：`apt remove` / `dnf remove` 只删 `/usr` 下的程序文件，
  **用户数据会保留在 `~/.de`**（会话历史、权限、日志，以及含 API key 的
  `conf.ini`）——这是有意为之，重装/升级不会丢会话。彻底清理：
  ```bash
  de purge          # 二次确认后删除 $DE_HOME；de purge --yes 跳过确认
  # 或直接：rm -rf ~/.de
  ```
  deb 卸载时也会打印数据位置与清理提示。

### 升级到新版本

**不用卸载重装，数据也不会丢。** 安装目录（`/usr/...` 或解包目录）里只有程序文件；
你的数据（`~/.de`：含 API key 的 conf.ini、会话库、权限/信任、日志）与之分离，
任何升级方式都不会触碰它。

```bash
# 1) 出/拿到新包（注意先把 CMakeLists.txt 的 VERSION 改掉再打包，deb 版本号来自它）
scripts/package-deb-rpm.sh          # 或 scripts/package-portable.sh

# 2) 覆盖安装（选一个）
sudo apt install ./de-agent_新版本_amd64.deb     # deb：同路径直接覆盖
tar xzf de-agent-新版本-linux-x86_64.tar.gz -C 旧包目录/   # 绿色包：解到同一目录覆盖

# 3) 重启后端，让新二进制生效（老进程一直在内存里跑旧代码）
de restart
# 源码树开发：重新 cmake --build build -j 后同样 de restart 即可
```

注意：

- `de init` 是幂等的，**不会**用 example 覆盖你已有的 `conf.ini`；新版本如果新增了
  配置项，需要自己对照 `<share>/de/conf/app.ini.example` 补进现有 conf.ini。
- 手动 `cp` 替换正在运行的后端二进制会报 `Text file busy`——先 `de stop` 再替换
  （apt/tar 覆盖的是新文件、旧进程仍映射旧 inode，不受影响）。
- 升级前后 `de status` / `de doctor` 各看一眼即可确认路径与健康状态。

## 使用

安装完成后，在**任意目录**运行 `de` 即可：需要时它会自动拉起后端守护进程，
然后进入终端客户端（填实际安装位置，如 `./de` 或 `<prefix>/bin/de`）。

```bash
de
```

### 让它干活

工具的工作目录 = **你启动 `de` 时所在的目录**。想到哪个项目上工作，先 `cd` 过去再运行：

```bash
cd ~/my-project
de
> 帮我读一下 README，并把 test 目录补一个用例
```

也可以在会话里用 `/cwd <dir>` 切换，`/status` 可查看当前 cwd、会话与后端状态。

### 你会看到的交互

- **流式输出**：回复逐 token 显示；等待模型/工具时底部有转圈动画（如 `⠋ thinking...`）。
- **工具调用**：`read`/`write`/`edit`/`bash` 以折叠块展示；`edit` 会渲染 `+/-` diff。
- **逐次确认**：默认 `confirm` 模式下，写文件与执行命令会先问
  `[y]es once / [a]lways this session / [N]o`，用 `/permissions` 调整。
- **中断**：生成过程中按 `Esc` 可停止当前任务。
- **回合汇总**：每次任务完成后，回复下方最后一行显示本次运行的墙钟耗时与 token
  用量（根模型各轮 + 本次运行内完成的子 Agent），形如
  `┗ task done in 12.3s · 15.2k tokens (prompt 12.0k · completion 3.2k · cache hit 9.0k)`。

### 常用指令

输入 `/` 会实时提示候选，`Tab` 补全，`/help` 看全部、`/hotkeys` 看快捷键。最常用的：

```text
/new              新会话
/resume           选择历史会话续聊（/sessions 查看列表）
/cwd [dir]        查看/切换工具工作目录
/permissions      查看/切换权限模式（confirm|workspace|full）
/tools            查看可用工具（coding + MCP）
/agents           查看子 Agent 开关、定义与运行统计（含 token 用量）
/status           后端健康 + 当前会话
/quit             退出
```

完整指令与说明见下文「终端 coding 客户端」一节。

### 服务管理

后端是后台守护进程，可随时管理：

```bash
de status     # 后端是否在运行
de restart    # 重启后端
de logs       # 跟踪后端日志（de logs cli 看客户端日志）
de stop       # 停止后端
```

### 数据在哪

会话历史（SQLite）、权限、日志都放在 `${DE_HOME:-$XDG_DATA_HOME/de:-$HOME/.de}`，
与安装目录分离：换新版本只需替换安装目录，历史会话仍在。`de doctor` 会打印当前
解析出的全部路径。

## 快速开始

### 1. 初始化 submodule

```bash
git submodule update --init --recursive
```

### 2. 构建 C++

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

### 3. 配置 DeepSeek API Key

复制配置模板并填写：

```bash
cp conf/app.ini.example conf/conf.ini
```

在 `conf/conf.ini` 中设置：

```ini
[deepseek]
api_key = sk-your-deepseek-api-key
base_url = https://api.deepseek.com
model = deepseek-v4-flash
reasoning_model = deepseek-v4-pro
```

也可以不写文件，改用环境变量：

```bash
export DEEPSEEK_API_KEY=sk-your-deepseek-api-key
```

### 4. 启动 C++ 后端

```bash
./build/aiagent_app --config conf/conf.ini --db build/db/aiagent_tasks.db
```

默认端口：

- A2A：`http://127.0.0.1:18080/`
- 本地 HTTP 后端（终端 CLI 使用）：`http://127.0.0.1:18081/`

### 5.（可选）启用 MCP 工具

构建 MCP Server：

```bash
cmake -S vendor/mcp-extension-platform \
  -B vendor/mcp-extension-platform/build \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build vendor/mcp-extension-platform/build -j
```

再次启动 `aiagent_app`，终端出现 `MCP tools loaded: N` 即表示已启用。

### 6. 终端 coding 客户端

安装 `de` 快捷命令（只需一次）：

```bash
scripts/install-de.sh
```

之后从任意目录输入 `de`：

```bash
de
```

首次运行会自动把 C++ 后端作为守护进程启动，然后进入终端客户端。后端保持后台运行，
可以再开多个终端分别输入 `de` 连接同一个后端；某个 CLI 退出不会影响其他终端。
coding 工具（read/write/edit/bash）以“你启动 `de` 时所在的目录”为工作目录，
每个 CLI 会把自身 cwd 随请求带给后端。

`edit` 支持一次改多处：除旧的 `{path, oldText, newText}` 形式外，还可以给
`edits: [{oldText, newText}, ...]`（对齐 pi）。多块语义是**原子**的——每个
`oldText` 必须在**原文**中恰好出现一次、各块区间不重叠，任何一块不满足就整体
拒绝、文件不变（错误信息带 `edits[i]` 序号便于模型自纠）。系统提示词也要求
模型把"同一文件的多处独立改动"合成一次 `edit` 调用、把互不依赖的工具调用放在
同一轮，以减少模型往返次数。

服务管理：

```bash
de stop      # 停止后端守护进程
de status    # 查看后端状态
de restart   # 重启后端
de logs      # 查看后端日志
```

常用命令（输入 `/` 会实时显示候选，`Tab` 补全，`/help` 查看全部、`/hotkeys` 查看快捷键）：

```text
/new              开始新会话
/resume [id|idx]  查看/选择一个历史会话续聊（/sessions、/open 为别名）
/session          查看当前会话统计（消息数、字符数、压缩信息）
/name <title>     重命名当前会话
/clone            复制当前会话为新会话并切换过去
/fork             从当前会话最新点 fork（等同 copy）
/delete [--yes]   删除当前会话（需确认，或显式 --yes）
/compact          把当前会话历史压缩为结构化摘要
/status           查看后端健康状态与当前会话
/tools            列出 Agent 可用工具（coding + MCP）
/agents           查看子 Agent 开关、定义与运行统计（含 token 用量）（/api/v1/agents）
/cwd [dir]        查看/切换工具工作目录
/permissions [m]  查看/切换当前工作路径的权限模式（confirm|workspace|full|list|reset|show）
/review prepare [feature] [--base <ref>]  生成 review pack（编码会话）
/review cancel    取消当前 review 回合（已写 draft 保留）
/review setup     查看 reviewer 如何检测/同意项目验证命令
/clear            清屏
/help             显示全部指令
/hotkeys          显示键盘快捷键
/theme [dark|light]  切换配色主题（写入 conf/cli.ini 的 [ui] theme）
/quit             退出（/exit 同义）
```

命令列表由 `cli/src/app/commands/` 的注册表统一生成，`/help`、候选提示与实际实现
不会漂移。会话标题（`/name`）持久化在数据库同目录的 `aiagent_sessions.json`。

流式输出期间可按 `Esc` 中断当前任务；CLI 会调用后端的取消接口，Agent
会在下一次模型轮次或工具执行前停止。

等待输出时会显示一个转圈动画（如 `⠋ thinking...`、`⠙ bash running...`，
超过 1 秒带计时），输出一到即自动清掉，便于区分“卡死”与“在等待”。
该动画只在 TTY 上绘制，管道/重定向时自动关闭。

界面还做了轻量化美化（对齐 pi）：输入区是上下两条 `─` 横线夹住的输入框，
行首只有 `❯ `；发送后的用户消息渲染成整块底色「长方体」；助手回复支持轻量
markdown（标题/列表/引用/行内代码/代码块）；工具调用以 `┏/┃/┗` 块展示
（失败为红，`edit` 会渲染 `+/-` diff）；cwd/分支/权限模式/会话等状态信息常驻
在终端最底部一行（无颜色时用户消息退化为 `▌ 文本`）。
颜色按终端能力自动降级（truecolor → 256 → 无色），`NO_COLOR=1` 可强制无色。
多行输入：`Alt+Enter`（或行尾 `\` 回车）换行，回车提交；输入过长会自动折行，
超过 8 行时在边框提示 `↑/↓ n more`。

输入行里按 `Ctrl+G` 会把当前内容写入临时 `prompt.md`，交给外部编辑器
（`--editor` > `conf/cli.ini` 的 `editor.external` > `$AIAGENT_EDITOR` >
`$VISUAL` > `$EDITOR`，默认 `nano`，Windows 为 `notepad`）编辑，保存退出后
内容回填；编辑器启动失败或非 0 退出时保留原输入。可复制
`conf/cli.ini.example` 为 `conf/cli.ini` 持久化设置。

CLI 启动参数：

```bash
--backend <url>    本地后端地址（默认 http://127.0.0.1:18081）
--session <id>     直接续聊指定会话
-c, --continue     继续最近一次会话
-r, --resume       启动时选择要续聊的会话
--name <title>     为新会话设置显示名
--cwd <dir>        工具工作目录（默认当前目录）
--editor <cmd>     Ctrl+G 使用的外部编辑器（默认见上）
--cli-config <f>   CLI 设置文件（默认 conf/cli.ini，可用 AIAGENT_CLI_CONFIG 覆盖）
--role <role>      coding | reviewer（默认 coding；已有会话不能中途切换）
--prompt <text>    启动后自动发送一条消息
--review-pack <p>  为 review pack 启动 reviewer 会话（等价 de review <p>）
--review-resume <id>  从 checkpoint 恢复未完成的 review
```

也可以直接指定会话进入：

```bash
de --session <session-id>
```

不使用 `de` 时，仍可手动启动后端和 CLI：

```bash
./build/aiagent_app --config conf/conf.ini --db build/db/aiagent_tasks.db
node cli/cli.mjs --backend http://127.0.0.1:18081
```

## 代码审核 Agent（reviewer）

审核是**独立顶层会话**，不是 `task` 子 Agent：独立 history、独立系统提示词、
路径受限工具集；与编码会话只通过 review pack / review report 文件交接。

```bash
# 1) 编码会话或外部 Agent 生成 pack
/review prepare skill-loader --base master
# 或外部编码方（不要求是 de）：
de review prepare --feature skill-loader --base master \
  --request-file .de/review/request.md --agent claude-code --json

# 2) 新终端启动独立 reviewer（路径可省略）
de review latest                       # 打开 docs/review/inbox 里最新的 pack
de review skill-loader                 # 打开该 feature 最新的 pack
de review docs/review/inbox/skill-loader-20261101-1430.json

# 3) reviewer 写报告后，回到编码会话按报告修复，再生成带 --previous-report 的第二轮
```

review pack 记录：base/head、tracked diff、untracked 文件（敏感文件只记路径）、
diff/untracked sha256、producer、request 文件/文本、上下文路径 hash、声明的
验证 argv。pack 之后代码一变，reviewer 的 `review_context` 就会返回 `stale`，
此时只能写 `blocked` 报告。

reviewer 工具集固定为 `read`/`grep`/`glob`/`ls` + 审核工具：

- `review_context`：manifest/plan/diff/untracked/session（P1 增加 delta）；
  manifest 返回 `coverage`（covered/uncovered/omitted）与预算，`plan` 返回按目录
  分组的覆盖计划与 focus。
- `review_read`：批量按行读真实文件与调用方；读完整文件会记入覆盖账本。
- `review_git`：只读 `diff/log/show/blame/rev-parse/ls-files/status`；拒绝 shell、
  checkout/reset/restore/stash/clean、reflog 与其他分支/未来提交。
- `review_scout`（P1，`[review] scout_max=1–4`）：最多 4 个只读分片；scout 只能
  读文件/查 git，不能写、不能执行命令、不能写报告；父 reviewer 必须重读证据，
  scout 覆盖不替代父 coverage。
- `review_verify`：只执行 pack 或 `.de/review.json` 声明的 argv（无 shell），
  执行前后比较源码 digest；源码被测试改动会返回 `source_modified=true`。
- `review_test_write`：只允许新建测试或向既有测试 append，不能覆盖/删除测试，
  更不能写源码/普通配置。
- `review_setup`：检测/展示/写入 `.de/review.json`；`save` 每次都要完整 argv 审批，
  用户同意记录绑定配置 sha256，配置被改动后旧同意失效。
- `review_report`：固定写到 `docs/review/<feature>-<YYYYMMDD-HHMM>.md`；
  final 报告必须有 verdict，且 finding/验证/测试文件与状态记录一致。
  有未覆盖文件时 final 只能写 `blocked`，不能静默漏审。

第二轮（P1）：生成 pack 时带 `--previous-report <上一份报告>`，reviewer 用
`review_context(mode=delta)` 看两轮之间的文件变化，并沿用旧 finding id 标记
`resolved` / `still_open` / `regressed`；新问题从旧 id 之后编号。

受控分片（P1）：`[review] scout_max=1–4` 时 reviewer 才能看到 `review_scout`；
默认 `0` 关闭。scout 是只读候选来源，父 reviewer 必须重读证据后再写报告。

后台审核（P1，需 `[review] background_enabled=true`）：

```bash
de review --background docs/review/inbox/<pack>.json [--session <coding-session>]
de review status [job_id]
de review cancel <job_id>
de review attach <job_id>
```

后台任务使用独立 reviewer 会话与工具实例；同一仓库同时只跑 1 个任务，排队上限
`max_queued_jobs`；同 repo+feature+branch 的新 pack 会顶掉旧的 queued/running 任务
（`superseded`）。后端重启后 running 任务标 `interrupted`，可从 checkpoint
`--resume`。后台只自动执行“用户已同意的 project 验证命令”；pack 特有命令、
`review_test_write`、`review_setup(save)` 会拒绝并要求写进 `unverified`。
完成后向发起会话推送 `review_ready` 事件（无活跃流时用 `de review status` 查询）。

`[review] verify_mode=copy` 时，`review_verify` 在 `git worktree` 临时副本中执行声明
的 argv（叠加 working-tree diff 与 untracked 文件），测试改副本不会污染原工作区；
含 submodule 的仓库暂不支持 copy。

没有通用 `write`/`edit`/`bash`/`task`/MCP 工具；`full` 权限下 reviewer 也不能读
仓库外路径。`[review] enabled=false` 时 coding 工具集不出现 `review_handoff`，
`de review` 在启动前明确报错。配置项见 `conf/app.ini.example` 的 `[review]`。

报告默认落在本地 `docs/`（该目录已被 gitignore），不自动提交。

## 本地后端 API（终端 CLI 使用）

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `POST` | `/api/v1/chat` | 同步对话 |
| `POST` | `/api/v1/chat/stream` | SSE 逐 token 流式对话 |
| `POST` | `/api/v1/chat/cancel` | 取消指定 session 的当前流式任务 |
| `POST` | `/api/v1/chat/approve` | 回传工具审批决定 `{session_id, tool_call_id, decision}` |
| `POST` | `/api/v1/sessions/:id/compact` | 手动压缩指定会话历史 |
| `GET` | `/api/v1/sessions/:id/compaction` | 查询最近一次压缩记录 |
| `PATCH` | `/api/v1/sessions/:id` | 设置会话显式标题（`/name`） |
| `DELETE` | `/api/v1/sessions/:id` | 删除会话及其全部任务 |
| `POST` | `/api/v1/sessions/:id/clone` | 复制会话为新会话（`/clone` `/fork`） |
| `GET` | `/api/v1/sessions/:id/stats` | 会话统计（消息数/字符数/任务数/压缩） |
| `GET` | `/api/v1/sessions` | 会话列表（`id` + `title` + `updated_at`，按最近使用排序） |
| `GET` | `/api/v1/sessions/:id/messages` | 会话历史消息 |
| `GET` | `/api/v1/tools?session_id=<id>` | 按会话角色返回工具清单（coding + MCP / 路径受限 reviewer） |
| `GET` | `/api/v1/reviews/config` | review 总开关 |
| `POST` | `/api/v1/reviews` | 创建 review pack（session 可空；request 文件/文本 + 声明 argv） |
| `GET` | `/api/v1/agents` | 子 Agent 开关/定义/运行统计（含 token 用量；关闭时返回 `{"enabled":false}`） |
| `GET` | `/api/v1/permissions` | 权限配置列表（默认模式 + 显式路径） |
| `GET` | `/api/v1/permissions/effective?path=<abs>` | 查询某路径的有效权限模式 |
| `PUT` | `/api/v1/permissions` | 设置某路径的权限模式 `{path, mode}` |
| `DELETE` | `/api/v1/permissions?path=<abs>` | 重置某路径为默认 |
| `GET` | `/health` | 健康检查 |

同步对话示例：

```bash
curl -X POST http://127.0.0.1:18081/api/v1/chat \
  -H 'Content-Type: application/json' \
  -d '{"message":"你好"}'
```

## 日志

日志基础设施位于 `include/aiagent/log.hpp` / `src/log.cpp`，零第三方依赖，
支持文件与可选 stdout、线程安全与 trace/debug/info/warn/error/fatal 级别。

`de` 启动后会分别写入：

```text
.runtime/backend.log          # 结构化 INFO/ERROR 日志
.runtime/backend-console.log  # 后端 stdout/stderr 原始输出
.runtime/cli.log              # CLI 事件日志
```

P1 后，`backend.log` 可串联 本地后端 / AgentRuntime / DeepSeek 的
请求生命周期：`event=chat_start → request_start → request_end → chat_end`，
并记录耗时、session、reply 大小与 DeepSeek 错误/重试。

P2 后，`tools` / `mcp` 模块还会记录 read/write/edit/bash 与
MCP `tools/list` / `tools/call` 的执行时长、输出大小和退出状态。

P3 后，CLI 也会写文件日志；`de` 会把自己的 `.runtime`
传给 CLI，方便统一查看。

P4 后，日志支持线程级 `req=` request id 串联 后端 → Agent → DeepSeek，
自动脱敏 `sk-...` API Key，并按大小轮转文件。

查看结构化日志：

```bash
de logs
de logs cli
```

配置：

```ini
[logging]
level = info
file = .runtime/backend.log
console = false
max_size_bytes = 10485760
max_files = 5
```

也可用环境变量或参数覆盖：

```bash
AIAGENT_LOG_LEVEL=debug de
de --log-level debug --log-file /tmp/debug.log
```

## 上下文压缩

当一次请求的上下文 token 超过 `context_window - reserve_tokens` 时，Agent
会把较早的消息交给模型生成结构化摘要（Goal / Progress / Decisions / Next
Steps 等六段），只保留最近 `keep_recent_tokens` 的消息，摘要作为一条 user
消息插在 system 之后。切点只落在 user / assistant 上，不会切断
`tool_calls` 与 `tool` 结果的配对。

```ini
[compaction]
enabled = true
context_window = 1000000   ; 模型真实窗口（deepseek-flash / v4-pro）
reserve_tokens = 200000    ; 给一次输出 + 摘要预留 → 触发线 80 万
compact_at_tokens = 0      ; 独立软阈值；0 = 只用「窗口 - 预留」
keep_recent_tokens = 60000 ; 压缩后保留的最近上下文
min_gain_ratio = 0.3       ; 收益下限：压完没降到 70% 以下就放弃
```

说明：

- `context_window` 请按实际模型填写（deepseek-flash / deepseek-v4-pro 为
  `1000000`）。实测：单轮耗时由「这一轮思考/输出多少」主导，与上下文大小关系
  很弱；反而压缩会打破前缀缓存（cacheRead 单价约为 input 的 1/50）并丢掉上下文，
  所以默认压得很少（触发线 80 万）。
- `compact_at_tokens` 是独立软阈值：填了则与「窗口 - 预留」取较小者，想「每轮更轻」
  可以调小（例如 `300000`），代价是更早丢上下文。
- `keep_recent_tokens` 必须显著小于触发线；启动时会检查，太贴近则夹到阈值的 1/3
  并在日志里 warning（否则会退化成"每轮都要再压一次"）。
- `min_gain_ratio` 是收益下限：压缩后没明显变小就保持原历史，记
  `event=compaction_skipped reason=low_gain`，不再白花一次摘要调用。
- 流式请求会自动开启 `stream_options.include_usage`，用真实 usage 计量；
  拿不到 usage 时退回按字节估算。触发口径只算 **prompt**（不含本轮 completion），
  避免一次大写入把估算顶过阈值。每轮日志会记录
  `prompt_tokens/completion_tokens/cache_hit_tokens`，便于观察缓存命中与成本。
- 若模型返回上下文超长错误或 `finish_reason=length`，会强制压缩并
  **重试一次**；重试仍失败则向上报错（不会无限重试）。
- 摘要会附加 `<read-files>` / `<modified-files>` 清单，压缩后仍能记住
  动过哪些文件，并在后续压缩中累积。
- 已有旧摘要时使用 UPDATE 模板做**增量合并**，保留旧信息、补充新进展。
- 流式期间会推送 `compaction` 事件（`tokens_before`/`tokens_after`），
  终端客户端会显示压缩提示。
- 压缩只作用于「本次请求构造的消息窗口」，SQLite 中仍保留完整历史，
  便于回溯。
- 手动 `/compact`（或 REST 接口）会把压缩后的更短历史作为新任务落库，
  后续请求直接使用；自动压缩仍为请求内内存操作。

## 项目指令（AGENTS.md）

终端 Agent 会在每次请求时按 CLI 的工作目录自动加载 `AGENTS.md`，并注入
system prompt；项目指令不会写入会话历史。查找顺序从当前工作目录向父目录
回溯，找到最近的 `.git` 根后停止；如果工作目录在子目录，仓库根和子目录的
`AGENTS.md` 会一起加载，离工作目录越近的优先级越高。

```ini
[instructions]
agents_md = true
agents_md_file_name = AGENTS.md
agents_md_override_file_name = AGENTS.override.md
agents_md_claude_fallback = false
agents_md_search_parents = true
agents_md_max_files = 8
agents_md_max_file_bytes = 16384
agents_md_max_total_bytes = 32768
agents_md_follow_symlinks = false
agents_md_max_depth = 16
```

说明：

- 终端通过 `--cwd` 或 `/cwd` 切换工作目录；切换后，聊天与 `/compact`
  都会使用新的工作目录加载项目指令。
- 会话最近一次 cwd 会持久化；重新打开会话时，如果启动参数没有显式指定
  `--cwd`，CLI 会自动恢复到该目录，`/session` 可查看后端记录的 cwd 与
  实际加载的 `AGENTS.md` 来源。
- 默认不跟随符号链接；文件过大时按 UTF-8 安全截断，并限制文件数与总字节数。
- 同一进程内按文件 mtime/size 缓存已读取内容，文件变化后自动重新加载。
- 项目指令只对当前项目生效，不能覆盖 Agent 的基础安全规则。
- 同一目录存在 `AGENTS.override.md` 时压制 `AGENTS.md`；两者都缺失时，
  可配置 `agents_md_claude_fallback = true` 回退读取 `CLAUDE.md`（默认关闭）。
- 项目指令渲染为 `<project_context>` / `<project_instructions path="...">`；
  内容里的闭合标签会被中和，避免提前结束 section。
- 流式、同步 `/api/v1/chat` 与 A2A `SendMessage` 共用同一套 system message
  组装，项目指令与 cwd 在三条路径上都生效。

### 系统提示词覆盖（用户级）

主 Agent 的基础提示词默认按本次请求的工具集动态生成（工具清单 + 工具贡献规则）。
需要实验或个性化时，可在配置文件里覆盖：

```ini
[prompt]
; 留空 = 内置动态提示词；非空 = 读取文件替换 preamble/tools/rules。
system_prompt_file =
; 非空 = 作为 <addendum> 追加到基础提示词后、项目指令前。
append_system_prompt_file =
; 非空 = 注入 <harness_docs>，列出 <root>/README.md 与 <root>/docs。
; 只在用户询问 harness 本身（配置/权限/技能/MCP/打包/SDK）时读取。
harness_docs_root =
; 两个文件的字节上限；超过直接报错。
max_system_prompt_bytes = 32768
```

- 相对路径相对配置文件所在目录解析，避免启动目录不同导致读不到同一个文件。
- 覆盖只影响根 Agent；子 Agent 继续使用 `[agents.<name>]` 的
  `system_prompt_file`。
- 覆盖/追加不能绕过权限层；工具调用仍受权限模式、审批与 bash 策略约束。
- `harness_docs_root` 只注入路径指针（`README.md` / `docs`），不读正文；
  配置了但目录不存在/没有文档时启动期直接报错。
- 基础提示词按 section 渲染：preamble 无标签，`<tools>`、`<rules>`、
  `<harness_docs>`、`<addendum>`、`<environment>`、`<project_context>`、
  `<skills>`、`<delegation>` 各自有边界。

## Skills

技能是可复用的 Markdown 指令包。系统提示只注入技能目录（名称 + 描述），
模型在任务匹配时调用只读工具 `skill(name, file?)` 按需加载正文或技能目录内的
资源文件；正文不会预先塞进上下文。

```text
<skills-root>/
└── release-notes/
    ├── SKILL.md
    └── reference.md
```

`SKILL.md` 至少包含 `name` 与 `description`：

```markdown
---
name: release-notes
description: Write release notes from git history. Use when asked to summarize commits for a release.
---

# Release notes
...
```

发现根按优先级从高到低：

1. 项目级 `<project-root>/.de/skills`
2. `[skills] extra_roots`
3. 用户级 `<data_dir>/skills`（默认 `~/.de/skills`）
4. 内置 `<share>/de/skills`（源码树回退 `skills/`）

同名技能高优先级胜出。配置：

```ini
[skills]
enabled = true
project = true
user = true
builtin = true
extra_roots =
max_skills = 64
max_catalog_bytes = 8192
max_skill_bytes = 65536
follow_symlinks = false
```

终端用 `/skills` 查看当前工作目录实际加载的技能；后端接口为
`GET /api/v1/skills?path=<cwd>`。`skill` 工具只在当前工作目录存在技能时才会
出现在模型工具表与 `/tools` 中；没有技能时不会多出该工具。默认不跟随符号链接，
正文/资源超过 `max_skill_bytes` 时截断，并在结果尾部提示调大该配置（不再承诺可
直接 `read` 全文）。

## 权限模式

Agent 在某个工作路径下的行为由「权限模式」决定。终端用 `/permissions`
查看与切换；后端在模型发起工具调用时按模式裁决（拒绝会作为工具结果回填，
不会中断整轮对话）。

| 模式 | 读 | 工作区内写 | 工作区外写 | 命令 |
| --- | :---: | :---: | :---: | :---: |
| `confirm`（默认） | ✅ | 需确认 | ❌ | 需确认 |
| `workspace` | ✅ | ✅ | 仅系统临时目录，且写入需确认；其他 ❌ | 需确认 |
| `full` | ✅ | ✅ | ✅ | ✅ |

> `confirm` 的写/命令、`workspace` 的临时目录写与命令会发起**逐次人工确认**：
> 终端弹出 `[y]es once / [a]lways this session / [N]o`；超时（默认 120s）或
> 非交互调用方（A2A / 同步接口）按拒绝处理。
>
> `workspace` 额外放行**系统临时目录**（Linux 默认 `/tmp`，Windows 默认
> `%TEMP%`）：`read` / `ls` / `glob` / `grep` 可直接访问；`write` / `edit`
> 落在该目录时先审批，批准后才执行。`confirm` 不开放该例外；`full` 由
> `allow_outside` 直接放行。

- 权限按 **canonical 工作路径**存储，支持父目录就近继承；`full` 不参与继承。
- 数据存在**用户级目录**（默认 `~/.de/permissions.json`，目录按需创建），
  可用 `[storage] data_dir` / `$DE_HOME` / `$XDG_DATA_HOME` 覆盖。
- 模式可在 `[permissions]` 配置默认值与上限；`full` 需要 CLI 二次确认。

```ini
[permissions]
default_mode = confirm
max_mode = full
allow_full_access = true
```

终端命令：

```text
/permissions                    查看当前目录的有效模式（TTY 下打开选择器）
/permissions workspace          设置模式（confirm | workspace | full）
/permissions list               列出所有显式配置
/permissions reset              清除当前目录的显式配置（回到默认/继承）
/permissions show               打印有效模式与能力摘要
```

### bash 命令白/黑名单

在权限模式之上，用一份**用户可编辑的纯文本规则文件**给 `bash` 加命令级策略：
`deny` 永不执行（不可被确认绕过），`allow` 免确认；都不命中才回落到模式。

- 文件：默认 `~/.de/bash_policy.rules`（可用 `[storage] bash_policy_file` 覆盖）；
  **不存在时首次启动自动生成**一份带注释的默认规则，直接编辑即可。
- 格式：一行一条，`#` 注释，`deny`/`allow` 前缀；token 前缀匹配，`re:` 走正则。
- 判定顺序：`deny > allow > 模式`；`allow` 要求命令的**每个分段**都命中。

```text
# deny  mkfs
# deny  re: (curl|wget)[^|]*\|\s*(sh|bash)
# allow git status
# allow git diff
# allow ls
```

> 字符串匹配不是安全边界（`$IFS`、引号、别名等可绕过）；真正的安全靠
> `confirm`/`workspace` 的逐次确认或进程级沙箱。完整建议清单见
> `docs/planning/platform/权限模式规划.md` 附录。

### 项目级配置（`.de/`）

在**项目根**（最近的 `.git` 祖先）放一个 `.de/` 目录，即可对当前项目覆盖用户级
配置：

```text
<project>/.de/
  permissions.json     # 路径 -> 模式（同样格式）
  bash_policy.rules    # 命令白/黑名单（同样格式）
```

- **只读、按需存在**：有就生效，没有就忽略；后端**不会创建**它。
- **收紧优先**（未信任）：项目级只能把权限变**更严**（如全局 `workspace` → 项目
  `confirm`），不能放松（项目设 `full` 会被忽略）；`bash` 的 `deny` **总是叠加**，
  项目 `allow` 暂不生效。
- 项目根的判定：从工作目录向上找最近的 `.git`；非 git 项目则看当下目录是否有 `.de/`。
- 配置改动需**重启后端**生效。

**初始化与信任**：

```text
/permissions init     # 在当前项目根按需生成 .de/{permissions.json,bash_policy.rules,.gitignore}
```

- 进入项目时，若检测到项目 `.de/` 且尚未信任，CLI 会询问是否信任：
  **不信任 → 只能收紧；信任 → 允许项目级放宽**（如 `allow`、更宽的模式）。
- 信任决定记录在用户级 `~/.de/trust.json`，可用同一提示随时撤回。
- `.de/.gitignore` 内容为 `*`，默认不提交（信任决策不应进仓库）。

## 工具沙箱

> 文件工具的路径沙箱是「权限模式」在文件操作上的具体执行：`workspace` /
> `confirm` 下开启，`full` 下关闭（`policy.allow_outside`）。

`read` / `write` / `edit` 三个文件工具默认启用路径沙箱：路径会被
canonical 化（解析 `.`、`..` 与符号链接），解析结果必须落在「本次请求的
工作目录」或其子目录之内，否则直接拒绝并返回 `path escapes sandbox`。
这可以阻止模型通过 `../`、绝对路径或指向外部的符号链接越权读写文件。

```ini
[tools]
sandbox_enabled = true
; 额外放行的根目录，逗号分隔；相对路径相对工作目录解析。
sandbox_roots =
```

说明：

- 沙箱以「每次请求携带的 cwd」为根，所以终端 `/cwd` 切换目录后，新目录
  即成为允许范围；A2A/同步接口没有 cwd 时退回后端启动目录。
- `workspace` 下**系统临时目录**（Linux 默认 `/tmp`，Windows 默认 `%TEMP%`）
  是额外只读根：`read` / `ls` / `glob` / `grep` 直接放行；`write` / `edit`
  需要逐次审批，批准后由工具层放行。`confirm` 不开放该例外；非 full 的其他
  越界路径仍直接拒绝。
- 需要访问工作目录之外的其他目录时，用 `sandbox_roots` 显式放行，不要图省事
  关掉沙箱。
- **`bash` 工具不受路径沙箱约束**：它在工作目录下执行任意命令，仍可访问
  全盘。`confirm` / `workspace` 下 `bash` 走逐次审批，批准后命令本身不设限；
  文件沙箱是「文件工具」的边界，不是进程级沙箱。需要更强隔离请后续引入
  容器化/进程级隔离。
- 关闭沙箱（`sandbox_enabled = false`）已由权限模式取代：会被兼容映射为
  默认模式 `full`（不推荐），建议直接用 `/permissions`。

## A2A

Agent 通过 A2A v1.0 JSON-RPC 暴露能力：

- Agent Card：`http://127.0.0.1:18080/.well-known/agent-card.json`
- JSON-RPC 端点：`POST http://127.0.0.1:18080/`

支持 `SendMessage`、`GetTask`、`ListTasks`、`CancelTask` 等协议方法。

## 测试

运行 C++ 测试：

```bash
ctest --test-dir build --output-on-failure
```

运行 CLI 测试：

```bash
cd cli && npm test
```

当前测试不依赖真实 DeepSeek API：模型与 MCP 均使用 fake 实现验证链路。

## 命令行示例

直接与 DeepSeek 对话：

```bash
./build/aiagent_deepseek_cli --config conf/conf.ini "你好"
./build/aiagent_deepseek_cli --config conf/conf.ini --stream "介绍一下自己"
```

## 开发状态

当前已完成：

- A2A Agent 骨架与 HTTP/SSE 服务端
- DeepSeek 同步/流式客户端
- MCP 工具桥接与工具调用循环
- 本地 HTTP 后端与终端 CLI coding 客户端

后续可继续完善：

- 鉴权与多用户
- 结构化日志与监控
- 更完善的会话管理（删除/重命名）
- 多 Agent 编排
