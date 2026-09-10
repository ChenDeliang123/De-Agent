# ai-agent-cpp

一个基于 C++ 的 AI Agent 项目，支持通过 DeepSeek 进行对话、按需调用 MCP
工具，并以 A2A（Agent-to-Agent）协议对外提供服务；同时附带一个轻量的
Node.js Web 前端用于快速体验。

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
- 内置 Web Gateway（REST + SSE）与零 npm 依赖的 Node BFF 前端。

[a2a-protocol]: https://gitee.com/chen_dl/a2a-protocol
[mcp-extension-platform]: https://gitee.com/chen_dl/mcp-extension-platform

## 系统架构

```text
┌──────────────┐      ┌──────────────────┐      ┌──────────────────┐
│   Browser    │ ───▶ │    Node BFF      │ ───▶ │   Web Gateway    │
│              │      │  web/node_frontend│      │  C++ REST + SSE  │
└──────────────┘      └──────────────────┘      └────────┬─────────┘
                                                         │
┌──────────────┐                                          │
│ A2A Client   │ ─────────────────────────────────────┐   │
└──────────────┘                                      │   │
                                                      ▼   ▼
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
│   └── web/                    # Web Gateway
├── src/                        # 对应实现
├── tests/                      # C++ 测试
├── examples/                   # CLI 示例
├── cli/                        # 终端 coding 客户端（Node，零依赖）
├── de                          # 一键启动器（守护后端 + CLI）
├── scripts/                    # install/uninstall de
├── web/node_frontend/          # Node BFF + 静态前端
├── vendor/
│   ├── a2a-protocol/           # Git submodule
│   └── mcp-extension-platform/ # Git submodule
└── third_party/
```

## 环境要求

- Linux（当前开发与测试环境）
- CMake ≥ 3.21
- 支持 C++20 的编译器（GCC 11+ / Clang 14+）
- libcurl 开发库
- Node.js ≥ 18（仅运行 Web 前端时）
- Python 3（可选，MCP Python 插件需要）

CMake 配置阶段会自动获取 `cpp-httplib` 与 SQLite，需要可用的网络连接。

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
- Web Gateway：`http://127.0.0.1:18081/`

### 5. 启动 Web 前端

```bash
cd web/node_frontend
npm start
```

浏览器打开：

```text
http://127.0.0.1:3000/
```

如果 C++ Web Gateway 不是 `18081`：

```bash
AIAGENT_BACKEND_URL=http://127.0.0.1:18082 npm start
```

### 6.（可选）启用 MCP 工具

构建 MCP Server：

```bash
cmake -S vendor/mcp-extension-platform \
  -B vendor/mcp-extension-platform/build \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build vendor/mcp-extension-platform/build -j
```

再次启动 `aiagent_app`，终端出现 `MCP tools loaded: N` 即表示已启用。

### 7. 终端 coding 客户端

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

服务管理：

```bash
de stop      # 停止后端守护进程
de status    # 查看后端状态
de restart   # 重启后端
de logs      # 查看后端日志
```

常用命令：

```text
/new        开始新会话
/sessions   查看会话
/open <id>  续聊历史会话
/quit       退出
```

流式输出期间可按 `Esc` 中断当前任务；CLI 会调用后端的取消接口，Agent
会在下一次模型轮次或工具执行前停止。

也可以直接指定会话进入：

```bash
de --session <session-id>
```

不使用 `de` 时，仍可手动启动后端和 CLI：

```bash
./build/aiagent_app --config conf/conf.ini --db build/db/aiagent_tasks.db
node cli/cli.mjs --backend http://127.0.0.1:18081
```

## Web API

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `POST` | `/api/v1/chat` | 同步对话 |
| `POST` | `/api/v1/chat/stream` | SSE 逐 token 流式对话 |
| `POST` | `/api/v1/chat/cancel` | 取消指定 session 的当前流式任务 |
| `GET` | `/api/v1/sessions` | 会话列表（`id` + 可读 `title`） |
| `GET` | `/api/v1/sessions/:id/messages` | 会话历史消息 |
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
.runtime/web.log              # Node BFF 请求日志
```

P1 后，`backend.log` 可串联 WebGateway / AgentRuntime / DeepSeek 的
请求生命周期：`event=chat_start → request_start → request_end → chat_end`，
并记录耗时、session、reply 大小与 DeepSeek 错误/重试。

P2 后，`tools` / `mcp` 模块还会记录 read/write/edit/bash 与
MCP `tools/list` / `tools/call` 的执行时长、输出大小和退出状态。

P3 后，CLI 与 Node BFF 也会写文件日志；`de` 会把自己的 `.runtime`
传给 CLI，方便统一查看。

P4 后，日志支持线程级 `req=` request id 串联 Web → Agent → DeepSeek，
自动脱敏 `sk-...` API Key，并按大小轮转文件。

查看结构化日志：

```bash
de logs
de logs cli
de logs web
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
context_window = 64000
reserve_tokens = 16384
keep_recent_tokens = 20000
```

说明：

- `context_window` 请按实际模型填写（如 DeepSeek 128k 模型可填 `128000`）。
- 流式请求会自动开启 `stream_options.include_usage`，用真实 usage 计量；
  拿不到 usage 时退回按字节估算。
- 若模型返回上下文超长错误或 `finish_reason=length`，会强制压缩并
  **重试一次**；重试仍失败则向上报错（不会无限重试）。
- 摘要会附加 `<read-files>` / `<modified-files>` 清单，压缩后仍能记住
  动过哪些文件，并在后续压缩中累积。
- 压缩只作用于「本次请求构造的消息窗口」，SQLite 中仍保留完整历史，
  便于回溯。

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

运行 Node 前端测试：

```bash
cd web/node_frontend && npm test
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
- Web Gateway
- Node 前端

后续可继续完善：

- 鉴权与多用户
- 结构化日志与监控
- 更完善的会话管理（删除/重命名）
- 多 Agent 编排
