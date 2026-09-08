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

终端客户端连接 C++ Web Gateway，支持流式对话、工具调用过程展示与断点续聊：

```bash
cd cli
node cli.mjs --backend http://127.0.0.1:18081
```

常用命令：

```text
/new        开始新会话
/sessions   查看会话
/open <id>  续聊历史会话
/quit       退出
```

也可以直接指定会话进入：

```bash
node cli.mjs --backend http://127.0.0.1:18081 --session <session-id>
```

## Web API

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `POST` | `/api/v1/chat` | 同步对话 |
| `POST` | `/api/v1/chat/stream` | SSE 逐 token 流式对话 |
| `GET` | `/api/v1/sessions` | 会话列表（`id` + 可读 `title`） |
| `GET` | `/api/v1/sessions/:id/messages` | 会话历史消息 |
| `GET` | `/health` | 健康检查 |

同步对话示例：

```bash
curl -X POST http://127.0.0.1:18081/api/v1/chat \
  -H 'Content-Type: application/json' \
  -d '{"message":"你好"}'
```

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
