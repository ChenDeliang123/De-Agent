#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace aiagent::review {

// 审核模块专用的 argv 执行器：直接 exec，不经过 shell；支持超时、输出上限、
// 尾部保留（验证命令只回传尾部）与取消回调。git 采集使用头部保留 + 截断标记。
struct ArgvRunResult {
    bool started = true;
    bool timed_out = false;
    bool cancelled = false;
    bool truncated = false;
    int exit_code = -1;
    long duration_ms = 0;
    std::string output;
    std::string error;
};

ArgvRunResult RunArgvCapture(const std::vector<std::string>& argv,
                             const std::string& cwd,
                             long timeout_seconds,
                             std::size_t max_output_bytes,
                             bool keep_tail = false,
                             const std::function<bool()>& should_cancel = {});

}  // namespace aiagent::review
