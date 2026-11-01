#pragma once

#include <string>

namespace aiagent::agent {

// 生成 <harness_docs> 的内容：只给文档路径指针，不读取正文。
//
// root_value 为空时表示关闭该能力：output 清空并返回 true。
// 相对 root 相对 config_path 所在目录解析（避免受启动目录影响）；config_path
// 为空时相对进程当前目录。目录不存在，或既没有 README.md 也没有 docs/ 时
// 返回 false，并通过 error 返回可直接展示的配置错误。
bool BuildHarnessDocsInfo(const std::string& root_value,
                          const std::string& config_path,
                          std::string* output,
                          std::string* error);

}  // namespace aiagent::agent
