#pragma once

#include <string>

namespace aiagent {

// 用户级数据目录与常见文件位置。
//
// 解析优先级（后者依次回退）：
//   explicit_data_dir（[storage] data_dir）
//   > $DE_HOME
//   > $XDG_DATA_HOME/de
//   > $HOME/.de
// 若以上都不可用，data_dir 为空，调用方需自行回退（例如数据库同目录）。
// 目录本身按需创建，不强依赖预先存在。
struct AppPaths {
    std::string data_dir;
    std::string permissions_file;
    std::string bash_policy_file;
};

AppPaths ResolveAppPaths(const std::string& explicit_data_dir = {});

}  // namespace aiagent
