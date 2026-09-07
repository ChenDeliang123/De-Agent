# ai-agent-cpp

AI Agent 的 C++ 实现项目。第一版规划见
`docs/项目实现规划.md`（本地笔记，不入库）。

## M0：最小 A2A Agent 骨架

M0 复用本地 `vendor/a2a-protocol`，通过 `AgentServer` +
`AgentHttpServer` 暴露一个最小的 A2A Agent。

### 依赖

- CMake >= 3.21
- C++20 编译器（GCC 11+ / Clang 14+）
- libcurl 开发库
- 本地 `vendor/a2a-protocol` 仓库

### 构建与运行

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
