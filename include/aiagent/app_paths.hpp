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
    std::string trust_file;
    // 安装/运行期间派生的默认位置（仅当 data_dir 非空时有值）。
    std::string config_file;  // <data_dir>/conf/conf.ini
    std::string runtime_dir;  // <data_dir>/runtime
    std::string db_file;      // <data_dir>/aiagent_tasks.db
};

AppPaths ResolveAppPaths(const std::string& explicit_data_dir = {});

// 安装资源目录（share/de），用于查找 conf 模板与 MCP 组件。
//
// 解析优先级：
//   explicit_share_dir > $AIAGENT_SHARE_DIR > <exe_dir>/../share/de
// exe_path 为空时读 /proc/self/exe（Linux）；拿不到可执行路径且无显式值时返回空。
// 只做词法归一（不要求目录存在），便于源码树与安装态共用。
std::string ResolveShareDir(const std::string& explicit_share_dir = {},
                            const std::string& exe_path = {});

// 选择实际使用的后端配置文件，返回第一个存在的候选；都不存在时返回空。
//
// 解析顺序：
//   1) explicit_path（--config / $AIAGENT_CONFIG）—— 非空即原样返回（不存在由调用方报错）
//   2) <cwd>/conf/conf.ini            （源码树 / 手动运行）
//   3) <paths.data_dir>/conf/conf.ini （安装态用户配置）
//   4) <share_dir>/conf/conf.ini      （安装包只读模板）
// cwd 为空时用当前进程工作目录。
std::string ResolveConfigFile(const std::string& explicit_path,
                              const AppPaths& paths,
                              const std::string& share_dir,
                              const std::string& cwd = {});

}  // namespace aiagent
