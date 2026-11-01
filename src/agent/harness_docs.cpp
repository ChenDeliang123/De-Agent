#include "aiagent/agent/harness_docs.hpp"

#include <filesystem>
#include <sstream>
#include <system_error>

#include "aiagent/platform/paths.hpp"

namespace aiagent::agent {
namespace {

std::filesystem::path ConfigDirectory(const std::string& config_path,
                                      std::error_code* ec) {
    if (config_path.empty()) {
        return std::filesystem::current_path(*ec);
    }
    const std::filesystem::path absolute =
            std::filesystem::absolute(aiagent::platform::FromUtf8(config_path), *ec);
    if (*ec) {
        return {};
    }
    return absolute.parent_path();
}

}  // namespace

bool BuildHarnessDocsInfo(const std::string& root_value,
                          const std::string& config_path,
                          std::string* output,
                          std::string* error) {
    if (output != nullptr) {
        output->clear();
    }
    if (root_value.empty()) {
        return true;
    }

    std::error_code ec;
    std::filesystem::path root = aiagent::platform::FromUtf8(root_value);
    if (!root.is_absolute()) {
        const std::filesystem::path config_dir = ConfigDirectory(config_path, &ec);
        if (ec) {
            if (error != nullptr) {
                *error = "harness_docs_root: cannot resolve config directory: " +
                         ec.message();
            }
            return false;
        }
        root = config_dir / root;
    }
    const std::filesystem::path requested_root = root;
    root = std::filesystem::weakly_canonical(root, ec);
    if (ec || !std::filesystem::is_directory(root, ec)) {
        if (error != nullptr) {
            *error = "harness_docs_root is not a directory: " +
                     aiagent::platform::ToUtf8(requested_root);
        }
        return false;
    }

    const std::filesystem::path readme = root / "README.md";
    const std::filesystem::path docs = root / "docs";
    std::error_code readme_error;
    std::error_code docs_error;
    const bool has_readme =
            std::filesystem::is_regular_file(readme, readme_error) && !readme_error;
    const bool has_docs =
            std::filesystem::is_directory(docs, docs_error) && !docs_error;
    if (!has_readme && !has_docs) {
        if (error != nullptr) {
            *error = "harness_docs_root contains neither README.md nor docs/: " +
                     aiagent::platform::ToUtf8(root);
        }
        return false;
    }

    // 只注入路径指针；正文由模型在用户确实询问 harness 时再读。
    std::ostringstream content;
    content << "Read these only when the user asks about the harness itself "
               "(configuration, permissions, skills, MCP, packaging, SDK/API):\n";
    if (has_readme) {
        content << "- Main documentation: "
                << aiagent::platform::ToGenericUtf8(readme) << '\n';
    }
    if (has_docs) {
        content << "- Additional docs: "
                << aiagent::platform::ToGenericUtf8(docs) << '\n';
    }
    if (output != nullptr) {
        *output = content.str();
    }
    return true;
}

}  // namespace aiagent::agent
