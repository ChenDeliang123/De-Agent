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
./build/aiagent_app
```

默认监听 `http://127.0.0.1:18080/`，可用参数或环境变量覆盖端口：

```bash
./build/aiagent_app 18081
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
