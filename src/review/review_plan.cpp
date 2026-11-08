#include "aiagent/review/review_plan.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "aiagent/review/project_verify_config.hpp"

namespace aiagent::review {
namespace {

std::vector<std::string> SplitReviewPath(const std::string& path) {
    std::vector<std::string> parts;
    std::string current;
    for (const char c : path) {
        if (c == '/') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

std::string ReviewGroupKey(const std::string& path) {
    const auto parts = SplitReviewPath(path);
    if (parts.empty()) {
        return "other";
    }
    if (parts[0] == "src" && parts.size() > 1) {
        return parts[1];
    }
    if (parts[0] == "include" && parts.size() > 2 && parts[1] == "aiagent") {
        return parts[2];
    }
    if (parts[0] == "tests" && parts.size() > 1) {
        const std::string& second = parts[1];
        if (second.rfind("review", 0) == 0) {
            return "review";
        }
        if (second.rfind("skill", 0) == 0) {
            return "skills";
        }
        if (second.rfind("web", 0) == 0) {
            return "web";
        }
        return "tests";
    }
    if (parts[0] == "cli") {
        return "cli";
    }
    if (parts[0] == "docs" || parts[0] == "conf") {
        return "docs-config";
    }
    if (parts[0] == "vendor") {
        return "vendor";
    }
    if (parts[0] == "CMakeLists.txt" || parts[0] == "README.md" ||
        parts[0] == "AGENTS.md" || parts[0] == ".gitignore") {
        return "build-docs";
    }
    return "other";
}

std::vector<std::string> ReviewGroupFocus(const std::string& group) {
    if (group == "review") {
        return {"producer->consumer dispatch", "path traversal/symlink/TOCTOU",
                "argv/shell/allowlist", "state/report atomic write",
                "lifetime/cancel/join"};
    }
    if (group == "skills") {
        return {"discovery precedence", "frontmatter/limits", "path/symlink",
                "cache/TTL", "prompt injection"};
    }
    if (group == "agent") {
        return {"prompt assembly", "lifetime/concurrency", "tool visibility",
                "permissions", "UTF-8/truncation"};
    }
    if (group == "web") {
        return {"auth/role", "route/consumer", "background lifetime", "path/JSON",
                "error mapping"};
    }
    if (group == "cli") {
        return {"argv/shell", "path normalization", "review dir", "job state",
                "Windows quoting"};
    }
    if (group == "tools") {
        return {"path sandbox", "command allowlist", "output caps", "cache",
                "error paths"};
    }
    if (group == "tests") {
        return {"assert behavior", "negative paths", "fixtures", "platform"};
    }
    if (group == "docs-config") {
        return {"config consumed", "defaults", "docs/impl consistency", "examples"};
    }
    if (group == "build-docs") {
        return {"build/test wiring", "config", "docs"};
    }
    return {"contracts/callers", "boundaries/UTF-8", "lifetime/concurrency",
            "error paths"};
}

}  // namespace

ReviewScope BuildReviewScope(const ReviewPack& pack) {
    std::set<std::string> required;
    std::set<std::string> omitted;
    for (const auto& file : pack.omitted) {
        const std::string path = NormalizeRelativePath(file.path);
        if (!path.empty()) {
            omitted.insert(path);
        }
    }
    for (const auto& file : pack.changed_files) {
        const std::string path = NormalizeRelativePath(file.path);
        if (path.empty()) {
            continue;
        }
        if (file.binary) {
            omitted.insert(path);
            continue;
        }
        if (omitted.count(path) == 0) {
            required.insert(path);
        }
    }
    for (const auto& file : pack.untracked) {
        const std::string path = NormalizeRelativePath(file.path);
        if (path.empty()) {
            continue;
        }
        if (file.redacted || !file.reason.empty()) {
            omitted.insert(path);
            continue;
        }
        if (omitted.count(path) == 0) {
            required.insert(path);
        }
    }
    ReviewScope scope;
    scope.required.assign(required.begin(), required.end());
    scope.omitted.assign(omitted.begin(), omitted.end());
    return scope;
}

std::vector<ReviewPlanGroup> BuildReviewPlanGroups(
        const std::vector<std::string>& required) {
    std::map<std::string, std::vector<std::string>> buckets;
    for (const auto& path : required) {
        buckets[ReviewGroupKey(path)].push_back(path);
    }
    while (buckets.size() > 8) {
        auto smallest = buckets.end();
        for (auto it = buckets.begin(); it != buckets.end(); ++it) {
            if (it->first == "other") {
                continue;
            }
            if (smallest == buckets.end() || it->second.size() < smallest->second.size()) {
                smallest = it;
            }
        }
        if (smallest == buckets.end()) {
            break;
        }
        auto& other = buckets["other"];
        other.insert(other.end(), smallest->second.begin(), smallest->second.end());
        buckets.erase(smallest);
    }

    std::vector<ReviewPlanGroup> groups;
    for (auto& [key, files] : buckets) {
        std::sort(files.begin(), files.end());
        for (std::size_t start = 0; start < files.size(); start += 20) {
            ReviewPlanGroup group;
            group.group_id = key;
            if (files.size() > 20) {
                group.group_id += "#" + std::to_string(start / 20 + 1);
            }
            const std::size_t end = std::min<std::size_t>(files.size(), start + 20);
            group.files.assign(files.begin() + start, files.begin() + end);
            group.focus = ReviewGroupFocus(key);
            groups.push_back(std::move(group));
        }
    }
    std::sort(groups.begin(), groups.end(),
              [](const ReviewPlanGroup& a, const ReviewPlanGroup& b) {
                  return a.group_id < b.group_id;
              });
    return groups;
}

}  // namespace aiagent::review
