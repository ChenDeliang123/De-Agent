#pragma once

#include <string>
#include <vector>

#include "aiagent/llm/deepseek_types.hpp"

namespace aiagent::agent {

// 终端 coding agent 的系统提示词（唯一事实来源）。
//
// 提示词由两部分组成：
//   1) 跨工具的基础行为规则（实现 vs 分析、最小改动、批量调用、验证尺度）；
//   2) 当前请求真实激活的工具贡献的 snippet/guidelines（ToolSpec 里的
//      prompt_snippet / prompt_guidelines）。
//
// 为什么按工具集生成：工具列表可能是动态的（MCP/skill 可用性、权限过滤、
// 子 Agent 白名单）。写死工具名会让模型调用本次请求没有声明的工具；把工具
// 机制放在 ToolSpec 旁边，也让工具描述和提示词只有一个事实来源。
//
// 约定：
//   - prompt_snippet 为空的内置工具不进入提示词的工具清单（MCP 工具默认走
//     “Additional tools may be available” 兜底）。
//   - prompt_guidelines 会按加入顺序去重后追加到 Rules。
//   - override_prompt 非空时替换 preamble/tools/rules（用户级 SYSTEM.md）；
//     append_prompt 非空时作为 <addendum> 追加。
struct SystemPromptOptions {
    std::vector<llm::ToolSpec> tools;
    std::string override_prompt;
    std::string append_prompt;
};

std::string BuildCodingSystemPrompt(const SystemPromptOptions& options);
std::string BuildCodingSystemPrompt(const std::vector<llm::ToolSpec>& tools);

}  // namespace aiagent::agent
