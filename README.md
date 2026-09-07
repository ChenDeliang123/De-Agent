# ai-agent-cpp

AI Agent 的 C++ 实现项目。第一版规划见
`docs/项目实现规划.md`（本地笔记，不入库）。

依赖仓库以 Git submodule 方式引入：

- `vendor/a2a-protocol`
- `vendor/mcp-extension-platform`

## M0：最小 A2A Agent 骨架

M0 复用 `vendor/a2a-protocol` submodule，通过 `AgentServer` +
`AgentHttpServer` 暴露一个最小的 A2A Agent。

### 依赖

- CMake >= 3.21
- C++20 编译器（GCC 11+ / Clang 14+）
- libcurl 开发库
- 已初始化的 submodule

### 构建与运行

首次 clone 后先拉取子仓库：

```bash
git submodule update --init --recursive
```

然后构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

M2 起 `aiagent_app` 已经是 DeepSeek Agent，启动需要读取密钥配置：

```bash
./build/aiagent_app --config conf/conf.ini --db build/db/aiagent_tasks.db
```

默认监听 `http://127.0.0.1:18080/`，可用参数或环境变量覆盖端口：

```bash
./build/aiagent_app --config conf/conf.ini --port 18081
# 或
AIAGENT_PORT=18081 ./build/aiagent_app
```

启动后可访问：

- Agent Card：`http://127.0.0.1:18080/.well-known/agent-card.json`
- A2A JSON-RPC 端点：`POST http://127.0.0.1:18080/`

### 测试

```bash
ctest --test-dir build --output-on-failure
```

M0 smoke test 会启动一个随机端口 HTTP Server，验证 Agent Card
发现和 `SendMessage` 往返链路。

## M1：DeepSeek Client

M1 实现了 OpenAI/DeepSeek Chat Completions 客户端，支持：

- 非流式对话与 SSE 流式对话
- `tools` / `tool_calls` 序列化与解析
- DeepSeek `reasoning_content` 保留
- 配置解析，密钥不进入代码

### 配置

私有配置放在 `conf/conf.ini`，该文件已被 `.gitignore` 屏蔽；
仓库内的 `conf/app.ini.example` 是公开模板。也支持用环境变量
`DEEPSEEK_API_KEY` 覆盖或通过 `AIAGENT_CONFIG` 指定其他配置文件。

### 命令行示例

```bash
./build/aiagent_deepseek_cli --config conf/conf.ini "你好"
./build/aiagent_deepseek_cli --config conf/conf.ini --stream "用一句话介绍自己"
```

### 测试

`aiagent_llm_client` 覆盖配置解析、请求体构造、非流式响应解析与
流式 chunk 解析，不依赖真实 DeepSeek API。

## M2：DeepSeek Agent Runtime

M2 把 M1 的 DeepSeek Client 接到 A2A `SendMessage`：

- 新增 `AgentRuntime`，把 A2A 历史翻译成 LLM messages 并调用模型。
- 使用 SQLite `TaskStore` 持久化任务与消息历史。
- 同一 `contextId` 可继续多轮对话；A2A 终态任务不允许续写，
  因此后续轮次以新 task + 相同 `contextId` 发送。
- Agent Card 的技能改为 `deepseek-chat`。

启动：

```bash
./build/aiagent_app --config conf/conf.ini --db build/db/aiagent_tasks.db
```

可用 `AIAGENT_DB` 环境变量指定数据库路径。

`aiagent_agent_runtime` 测试使用 fake model 验证首轮/第二轮历史拼接与
模型调用，不依赖真实 DeepSeek API。

## M3：MCP 工具调用

M3 把已有的 MCP Extension Platform 作为子进程接入 Agent：

- `MCPClientBridge` 通过 stdio JSON-RPC 启动并管理 MCP Server。
- 自动执行 MCP 握手、`tools/list` 和 `tools/call`。
- `AgentRuntime` 支持完整工具循环：模型返回 `tool_calls` → 执行 MCP 工具 →
  回填 `tool` 消息 → 再次请求模型。
- `aiagent_app` 启动时若检测到 MCP Server 可执行文件，会自动加载工具。

先构建 MCP Server（产物在 submodule 内，已被其 gitignore 屏蔽）：

```bash
cmake -S vendor/mcp-extension-platform -B vendor/mcp-extension-platform/build -DCMAKE_BUILD_TYPE=Debug
cmake --build vendor/mcp-extension-platform/build -j
```

再启动 AI Agent，终端会打印 `MCP tools loaded: N`：

```bash
./build/aiagent_app --config conf/conf.ini --db build/db/aiagent_tasks.db
```

也可在 `conf/conf.ini` 的 `[mcp]` 段自定义：

```ini
[mcp]
binary = /path/to/mcp_server
plugins = /path/to/plugins
python_path = python3
request_timeout_seconds = 60
```

测试使用 fake MCP Server 和 fake model，验证 stdio 通信与完整工具调用循环。
