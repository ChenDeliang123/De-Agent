# AGENTS.md

本仓库是 A2A（Agent-to-Agent）协议 v1.0 的 C++ 实现项目。任何在本仓库中
工作的 agent 或协作者都必须遵守以下工作约定。

## 工作约定

1. 每次改动后，都必须创建一个对应的 git commit，以便后续追踪和回滚。
   - 一次逻辑改动对应一个 commit，提交信息简明描述本次改动内容。
   - 不要在仓库里积压多个未提交的改动。

2. 每次改动后，都必须编写或更新相关测试，并在交付给用户前，确保所有测试和验证全部通过。
   - 构建验证：`cmake --build build -j`
   - 测试验证：`ctest --test-dir build --output-on-failure`
   - 若改动会影响格式化风格，需按 `.clang-format` 重新格式化相关源码。

3. 每次改动成功后，都必须将改动内容写到 `项目改动内容.md`。
   - 在“改动历史”一节追加一条记录：日期、改动摘要、涉及文件。
   - 改动历史应与 git 提交一一对应。

4. 用户以“提问/讲解”方式交流（如“X 有什么用”“讲解一下 Y”“精读建议”等）时，
   自主判断该问答是否有长期参考价值，并据此决定是否沉淀到 `docs/` 目录下的一个
   或若干个 md 文件（可新建，也可更新既有文档）：
   - 有沉淀价值的（架构讲解、设计缘由、精读指南、易踩坑结论等）→ 写入 `docs/`;
   - 仅一次性、低复用价值的问答可不写，避免文档膨胀。
   - 由这类提问产生的 `docs/` 文件改动**不需要 git commit**，也无需在
     `项目改动内容.md` 记录（与约定 1、3 冲突时以本条为准；`docs/` 已在
     `.gitignore` 中，属本地笔记性质）。

## 代码约定：JSON 序列化

模型层的 `to_json` / `from_json` 采用分层选型，避免风格反复摇摆：

- **纯值对象**（`Part`、`Artifact`、`AgentInterface`、`AgentSkill` 等）：保持
  `struct` + 命名空间内自由 `to_json` / `from_json`。nlohmann 通过 ADL 找到
  它们，嵌套组合（如 `Task` 内的 `history`、`Message` 内的 `parts`）依赖此机制。
- **行为实体**（需要封装状态/不变量时，如未来长出生命周期逻辑的 `Task`）：
  升级为 `class`，提供成员 `ToJson()` / `Parse()`，同时在 `.cpp` 保留一行转发的
  自由 `to_json` / `from_json`，保证嵌套序列化与既有调用不受破坏。
- **信封/聚合根**（`JsonRpcRequest` / `JsonRpcResponse` / `AgentCard`）：
  `class` + 成员 `Parse()` / `ToJson()`，不提供自由函数。
- 新字段遵循 ProtoJSON camelCase，枚举使用 SCREAMING_SNAKE_CASE，时间戳使用
  ISO 8601 UTC；解析不合法输入抛 `core::JsonRpcException`（kInvalidParams）。
- 不为“风格统一”做一次性大规模迁移；某个类型出现行为需求时，再按上述规则
  单独升级并提交。
