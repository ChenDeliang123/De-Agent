#include "aiagent/skills/skill_loader.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include "aiagent/config.hpp"

namespace {

namespace fs = std::filesystem;

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

class TempDir {
public:
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() / ("aiagent-skills-" + std::to_string(stamp));
        fs::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void WriteFile(const fs::path& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
}

void WriteSkill(const fs::path& root, const std::string& directory, const std::string& content) {
    WriteFile(root / directory / "SKILL.md", content);
}

std::string SkillText(const std::string& name, const std::string& description,
                      const std::string& body = "# Steps\n\nRun the steps.\n") {
    std::string text = "---\n";
    if (!name.empty()) {
        text += "name: " + name + "\n";
    }
    text += "description: " + description + "\n---\n\n" + body;
    return text;
}

const aiagent::skills::SkillEntry* FindEntry(const aiagent::skills::SkillCatalog& catalog,
                                             const std::string& name) {
    for (const auto& entry : catalog.entries) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

void TestParseSkillsConfig() {
    std::string error;
    const auto parsed = aiagent::ParseSkillsConfig(
            "[skills]\n"
            "enabled = false\n"
            "project = false\n"
            "user = false\n"
            "builtin = false\n"
            "extra_roots = /a, ./b\n"
            "max_skills = 3\n"
            "max_catalog_bytes = 1024\n"
            "max_skill_bytes = 2048\n"
            "follow_symlinks = true\n",
            &error);
    Expect(parsed.has_value(), "skills config should parse");
    if (parsed.has_value()) {
        Expect(!parsed->enabled, "enabled should parse false");
        Expect(!parsed->project, "project should parse false");
        Expect(!parsed->user, "user should parse false");
        Expect(!parsed->builtin, "builtin should parse false");
        Expect(parsed->extra_roots.size() == 2 && parsed->extra_roots[0] == "/a" &&
                       parsed->extra_roots[1] == "./b",
               "extra_roots should split on commas");
        Expect(parsed->max_skills == 3, "max_skills should parse");
        Expect(parsed->max_catalog_bytes == 1024, "max_catalog_bytes should parse");
        Expect(parsed->max_skill_bytes == 2048, "max_skill_bytes should parse");
        Expect(parsed->follow_symlinks, "follow_symlinks should parse true");
    }

    const auto defaults = aiagent::ParseSkillsConfig("[deepseek]\nmodel = test\n", &error);
    Expect(defaults.has_value(), "missing skills section should use defaults");
    if (defaults.has_value()) {
        Expect(defaults->enabled, "enabled default should be true");
        Expect(defaults->project && defaults->user && defaults->builtin,
               "root switches should default to true");
        Expect(defaults->max_skills == 64, "max_skills default should be 64");
        Expect(defaults->max_catalog_bytes == 8192, "max_catalog_bytes default should be 8192");
        Expect(defaults->max_skill_bytes == 65536, "max_skill_bytes default should be 65536");
        Expect(!defaults->follow_symlinks, "follow_symlinks default should be false");
    }

    Expect(!aiagent::ParseSkillsConfig("[skills]\nmax_skills = 0\n", &error).has_value(),
           "zero max_skills should be rejected");
    Expect(!aiagent::ParseSkillsConfig("[skills]\nmax_skills = -1\n", &error).has_value(),
           "negative max_skills should be rejected");
    Expect(!aiagent::ParseSkillsConfig("[skills]\nmax_catalog_bytes = 65537\n", &error).has_value(),
           "oversized max_catalog_bytes should be rejected");
    Expect(!aiagent::ParseSkillsConfig("[skills]\nmax_skill_bytes = 1048577\n", &error).has_value(),
           "oversized max_skill_bytes should be rejected");

    TempDir temp;
    const fs::path config_path = temp.path() / "app.ini";
    WriteFile(config_path, "[skills]\nmax_skills = 2\n");
    const auto loaded = aiagent::LoadSkillsConfigFile(config_path.string(), &error);
    Expect(loaded.has_value() && loaded->max_skills == 2,
           "LoadSkillsConfigFile should read the [skills] section");
}

void TestDiscoveryAndPrecedence() {
    TempDir temp;
    const fs::path repo = temp.path() / "repo";
    const fs::path sub = repo / "sub";
    WriteFile(repo / ".git", "gitdir: .\n");
    fs::create_directories(sub);

    const fs::path project_skills = repo / ".de" / "skills";
    WriteSkill(project_skills, "project-only", SkillText("project-only", "project only"));
    WriteSkill(project_skills, "shared", SkillText("shared", "project shared"));
    WriteSkill(project_skills, ".hidden", SkillText("hidden", "hidden skill"));

    const fs::path extra = temp.path() / "extra";
    WriteSkill(extra, "extra-only", SkillText("extra-only", "extra only"));
    WriteSkill(extra, "shared", SkillText("shared", "extra shared"));

    const fs::path user = temp.path() / "user";
    WriteSkill(user, "user-only", SkillText("user-only", "user only"));
    WriteSkill(user, "shared", SkillText("shared", "user shared"));

    const fs::path builtin = temp.path() / "builtin";
    WriteSkill(builtin, "builtin-only", SkillText("builtin-only", "builtin only"));
    WriteSkill(builtin, "shared", SkillText("shared", "builtin shared"));

    aiagent::SkillsConfig config;
    config.extra_roots = {extra.string()};
    aiagent::skills::SkillLoader loader(config, {user.string()}, {builtin.string()});
    const auto catalog = loader.LoadCatalog(sub.string());

    Expect(catalog.entries.size() == 5, "catalog should contain five unique skills");
    Expect(!catalog.truncated, "catalog should not be truncated");

    const auto* shared = FindEntry(catalog, "shared");
    Expect(shared != nullptr, "shared skill should be present");
    if (shared != nullptr) {
        Expect(shared->source == aiagent::skills::SkillSource::kProject,
               "project skill should win duplicate name");
        Expect(shared->description == "project shared", "project skill description should win");
    }

    const auto* extra_only = FindEntry(catalog, "extra-only");
    Expect(extra_only != nullptr && extra_only->source == aiagent::skills::SkillSource::kExtra,
           "extra-only should come from extra root");
    const auto* user_only = FindEntry(catalog, "user-only");
    Expect(user_only != nullptr && user_only->source == aiagent::skills::SkillSource::kUser,
           "user-only should come from user root");
    const auto* builtin_only = FindEntry(catalog, "builtin-only");
    Expect(builtin_only != nullptr &&
                   builtin_only->source == aiagent::skills::SkillSource::kBuiltin,
           "builtin-only should come from builtin root");

    const std::string rendered = aiagent::skills::SkillLoader::RenderCatalog(catalog);
    Expect(rendered.rfind("Skills:\n", 0) == 0, "rendered catalog should start with header");
    Expect(rendered.find("- project-only: project only") != std::string::npos,
           "rendered catalog should contain project-only");
    Expect(rendered.find("- shared: project shared") != std::string::npos,
           "rendered catalog should contain the winning shared skill");
    Expect(rendered.find("- shared: extra shared") == std::string::npos,
           "rendered catalog should not contain the losing duplicate");
    Expect(aiagent::skills::SkillLoader::RenderCatalog({}).empty(),
           "empty catalog should render as empty text");
}

void TestFrontmatterVariants() {
    TempDir temp;
    const fs::path root = temp.path() / "skills";

    WriteSkill(root, "quoted",
               "---\n"
               "name: \"quoted\"\n"
               "description: \"Line one.\\nLine two.\"\n"
               "---\n\n"
               "# Body\n");
    WriteSkill(root, "folded",
               "---\n"
               "name: folded\n"
               "description: >\n"
               "  First line\n"
               "  second line\n"
               "---\n\n"
               "body\n");
    WriteSkill(root, "literal",
               "---\n"
               "name: literal\n"
               "description: |\n"
               "  First line\n"
               "  second line\n"
               "---\n\n"
               "body\n");
    WriteSkill(root, "fallback-name",
               "---\n"
               "description: fallback description\n"
               "---\n\n"
               "body\n");
    WriteSkill(root, "mismatch",
               "---\n"
               "name: declared-name\n"
               "description: declared description\n"
               "---\n\n"
               "body\n");
    WriteSkill(root, "bom",
               "\xEF\xBB\xBF---\r\n"
               "name: bom\r\n"
               "description: bom description\r\n"
               "---\r\n\r\n"
               "body\r\n");
    WriteSkill(root, "block-before-name",
               "---\n"
               "description: |\n"
               "  first line\n"
               "  second line\n"
               "name: actual-name\n"
               "---\n\n"
               "# Body\n"
               "real body\n");
    WriteSkill(root, "block-with-separator",
               "---\n"
               "description: |\n"
               "  first line\n"
               "  ---\n"
               "  second line\n"
               "name: separator-name\n"
               "---\n\n"
               "# Body\n"
               "real body\n");

    aiagent::SkillsConfig config;
    config.project = false;
    aiagent::skills::SkillLoader loader(config, {root.string()}, {});
    const auto catalog = loader.LoadCatalog(temp.path().string());
    Expect(catalog.entries.size() == 8, "all valid frontmatter variants should load");

    const auto* quoted = FindEntry(catalog, "quoted");
    Expect(quoted != nullptr, "quoted name should load");
    if (quoted != nullptr) {
        Expect(quoted->description == "Line one. Line two.",
               "escaped newline should normalize to one line");
    }
    const auto* folded = FindEntry(catalog, "folded");
    Expect(folded != nullptr && folded->description == "First line second line",
           "folded block scalar should join lines with spaces");
    const auto* literal = FindEntry(catalog, "literal");
    Expect(literal != nullptr && literal->description == "First line second line",
           "literal block scalar should normalize to one line");
    Expect(FindEntry(catalog, "fallback-name") != nullptr,
           "missing name should fall back to the directory name");
    Expect(FindEntry(catalog, "declared-name") != nullptr,
           "frontmatter name should win over directory name");
    Expect(FindEntry(catalog, "bom") != nullptr, "BOM/CRLF frontmatter should load");

    const auto* actual_name = FindEntry(catalog, "actual-name");
    Expect(actual_name != nullptr && actual_name->description == "first line second line",
           "a key after a block scalar must still be parsed");
    const auto* separator_name = FindEntry(catalog, "separator-name");
    Expect(separator_name != nullptr && separator_name->description == "first line --- second line",
           "an indented --- inside a block scalar must not close frontmatter");
    aiagent::skills::SkillContent separator_content;
    std::string separator_error;
    Expect(loader.LoadContent("separator-name", "", temp.path().string(), &separator_content,
                              &separator_error),
           "block scalar with an indented separator should load");
    Expect(separator_content.body == "# Body\nreal body",
           "frontmatter after an indented separator must not leak into the body");

    const fs::path invalid = temp.path() / "invalid";
    WriteSkill(invalid, "missing-description", "---\nname: missing-description\n---\nbody\n");
    WriteSkill(invalid, "bad-name", "---\nname: Bad_Name\ndescription: x\n---\nbody\n");
    WriteSkill(invalid, "unclosed", "---\nname: unclosed\ndescription: x\n");
    WriteSkill(invalid, "no-frontmatter", "# Body\n");
    WriteSkill(invalid, "empty-body", "---\nname: empty-body\ndescription: x\n---\n\n\n");
    WriteSkill(invalid, "binary",
               std::string("---\nname: binary\ndescription: x\n---\n\nbody") +
                       std::string(1, '\0') + "tail");

    aiagent::skills::SkillLoader invalid_loader(config, {invalid.string()}, {});
    const auto invalid_catalog = invalid_loader.LoadCatalog(temp.path().string());
    Expect(invalid_catalog.entries.empty(),
           "invalid skills should be skipped without failing the catalog");
}

void TestLimitsAndTruncation() {
    TempDir temp;
    const fs::path root = temp.path() / "skills";
    WriteSkill(root, "a", SkillText("a", "skill a"));
    WriteSkill(root, "b", SkillText("b", "skill b"));

    aiagent::SkillsConfig limited;
    limited.project = false;
    limited.max_skills = 1;
    aiagent::skills::SkillLoader limited_loader(limited, {root.string()}, {});
    const auto limited_catalog = limited_loader.LoadCatalog(temp.path().string());
    Expect(limited_catalog.entries.size() == 1, "max_skills should stop after one skill");
    Expect(limited_catalog.truncated, "max_skills truncation should be flagged");

    aiagent::SkillsConfig tiny_catalog;
    tiny_catalog.project = false;
    tiny_catalog.max_catalog_bytes = 1;
    aiagent::skills::SkillLoader tiny_loader(tiny_catalog, {root.string()}, {});
    const auto tiny = tiny_loader.LoadCatalog(temp.path().string());
    Expect(tiny.entries.empty(), "tiny catalog budget should omit every skill");
    Expect(tiny.truncated, "tiny catalog budget should be flagged");

    const std::string long_body(400, 'x');
    WriteSkill(root, "long", SkillText("long", "long skill", long_body));
    aiagent::SkillsConfig tiny_skill;
    tiny_skill.project = false;
    tiny_skill.max_skill_bytes = 80;
    aiagent::skills::SkillLoader tiny_skill_loader(tiny_skill, {root.string()}, {});
    aiagent::skills::SkillContent content;
    std::string error;
    const bool loaded =
            tiny_skill_loader.LoadContent("long", "", temp.path().string(), &content, &error);
    Expect(loaded, "truncated skill body should still load");
    if (loaded) {
        Expect(content.truncated, "oversized body should be marked truncated");
        Expect(content.body.find("truncated") != std::string::npos,
               "oversized body should carry a truncation marker");
    }
}

void TestCacheRefresh() {
    TempDir temp;
    const fs::path root = temp.path() / "skills";
    WriteSkill(root, "cached", SkillText("cached", "first version"));

    aiagent::SkillsConfig config;
    config.project = false;
    aiagent::skills::SkillLoader loader(config, {root.string()}, {});
    const auto first = loader.LoadCatalog(temp.path().string());
    const auto* first_entry = FindEntry(first, "cached");
    Expect(first_entry != nullptr && first_entry->description == "first version",
           "initial catalog should expose the first description");

    WriteSkill(root, "cached", SkillText("cached", "second version", std::string(200, 'y')));
    const auto second = loader.LoadCatalog(temp.path().string());
    const auto* second_entry = FindEntry(second, "cached");
    Expect(second_entry != nullptr && second_entry->description == "second version",
           "changed file should invalidate the cache");

    aiagent::skills::SkillContent content;
    std::string error;
    Expect(loader.LoadContent("cached", "", temp.path().string(), &content, &error),
           "changed skill content should load");
    Expect(content.body.find("yyyy") != std::string::npos,
           "changed body should be visible after cache invalidation");
}

void TestResources() {
    TempDir temp;
    const fs::path root = temp.path() / "skills";
    const fs::path skill = root / "resource-skill";
    WriteSkill(root, "resource-skill", SkillText("resource-skill", "resource skill"));
    WriteFile(skill / "reference.md", "reference body\n");
    WriteFile(skill / "scripts" / "run.sh", "echo hi\n");
    WriteFile(skill / "binary.bin", std::string("ab\0cd", 5));
    WriteFile(temp.path() / "secret.txt", "secret\n");

    aiagent::SkillsConfig config;
    config.project = false;
    aiagent::skills::SkillLoader loader(config, {root.string()}, {});
    aiagent::skills::SkillContent content;
    std::string error;

    Expect(loader.LoadContent("resource-skill", "", temp.path().string(), &content, &error),
           "skill body should load");
    Expect(content.body.find("# Steps") != std::string::npos, "skill body should contain markdown");
    Expect(content.entry.source == aiagent::skills::SkillSource::kUser,
           "entry should carry its source");
    Expect(content.path.find("SKILL.md") != std::string::npos,
           "body load should report the SKILL.md path");
    Expect(std::find(content.resources.begin(), content.resources.end(), "reference.md") !=
                           content.resources.end() &&
                   std::find(content.resources.begin(), content.resources.end(), "scripts/") !=
                           content.resources.end(),
           "body load should list direct resource entries");

    Expect(loader.LoadContent("resource-skill", "reference.md", temp.path().string(), &content,
                              &error),
           "relative resource should load");
    Expect(content.body == "reference body\n", "resource body should be returned verbatim");
    Expect(content.entry.name == "resource-skill", "resource result should carry the entry");
    Expect(content.path.find("reference.md") != std::string::npos,
           "resource load should report the resolved path");

    Expect(loader.LoadContent("resource-skill", "scripts/run.sh", temp.path().string(), &content,
                              &error),
           "nested resource should load");

    Expect(!loader.LoadContent("resource-skill", "../secret.txt", temp.path().string(), &content,
                               &error),
           "parent traversal should be rejected");
    Expect(!loader.LoadContent("resource-skill", (skill / "reference.md").string(),
                               temp.path().string(), &content, &error),
           "absolute resource paths should be rejected");
    Expect(!loader.LoadContent("resource-skill", "scripts", temp.path().string(), &content, &error),
           "directories should not be returned as resources");
    Expect(!loader.LoadContent("resource-skill", "binary.bin", temp.path().string(), &content,
                               &error),
           "binary resources should be rejected");
    Expect(!loader.LoadContent("resource-skill", "missing.md", temp.path().string(), &content,
                               &error),
           "missing resources should fail");

    Expect(!loader.LoadContent("unknown", "", temp.path().string(), &content, &error),
           "unknown skill should fail");
    Expect(error.find("available:") != std::string::npos,
           "unknown skill error should list available skills");

    Expect(!loader.LoadContent("resource-skill", "", (temp.path() / "missing").string(), &content,
                               &error),
           "invalid working directory should fail");
    Expect(error.find("invalid working directory") != std::string::npos,
           "invalid cwd error should be explicit");

    aiagent::SkillsConfig disabled;
    disabled.enabled = false;
    aiagent::skills::SkillLoader disabled_loader(disabled, {root.string()}, {});
    Expect(disabled_loader.LoadCatalog(temp.path().string()).entries.empty(),
           "disabled loader should return an empty catalog");
    Expect(!disabled_loader.LoadContent("resource-skill", "", temp.path().string(), &content,
                                        &error),
           "disabled loader should reject content loads");
    Expect(error.find("disabled") != std::string::npos, "disabled error should explain the switch");
}

void TestSymlinks() {
    TempDir temp;
    const fs::path real = temp.path() / "real";
    WriteSkill(real, "linked", SkillText("linked", "linked skill"));

    const fs::path root = temp.path() / "root";
    fs::create_directories(root);
    std::error_code ec;
    fs::create_directory_symlink(real / "linked", root / "linked", ec);
    if (ec) {
        std::cout << "SKIP: skill symlink checks: " << ec.message() << "\n";
        return;
    }

    aiagent::SkillsConfig config;
    config.project = false;
    aiagent::skills::SkillLoader no_follow(config, {root.string()}, {});
    Expect(no_follow.LoadCatalog(temp.path().string()).entries.empty(),
           "symlinked skill directory should be skipped by default");

    config.follow_symlinks = true;
    aiagent::skills::SkillLoader follow(config, {root.string()}, {});
    const auto catalog = follow.LoadCatalog(temp.path().string());
    Expect(catalog.entries.size() == 1, "follow_symlinks=true should load the symlinked skill");

    WriteFile(temp.path() / "outside.txt", "outside\n");
    ec.clear();
    fs::create_symlink(temp.path() / "outside.txt", real / "linked" / "outside-link.txt", ec);
    if (!ec) {
        aiagent::skills::SkillContent content;
        std::string error;
        Expect(!follow.LoadContent("linked", "outside-link.txt", temp.path().string(), &content,
                                   &error),
               "resource symlink escaping the skill directory should be rejected");
    }
}

void TestDuplicateWithinRoot() {
    TempDir temp;
    const fs::path root = temp.path() / "skills";
    WriteSkill(root, "a", SkillText("dupe", "from a"));
    WriteSkill(root, "b", SkillText("dupe", "from b"));

    aiagent::SkillsConfig config;
    config.project = false;
    aiagent::skills::SkillLoader loader(config, {root.string()}, {});
    const auto catalog = loader.LoadCatalog(temp.path().string());
    Expect(catalog.entries.size() == 1, "duplicate name should keep one entry");
    const auto* entry = FindEntry(catalog, "dupe");
    Expect(entry != nullptr && entry->description == "from a",
           "first directory in sorted order should win duplicate");
}

void TestStableRootBase() {
    TempDir temp;
    const fs::path stable = temp.path() / "stable";
    const fs::path other = temp.path() / "other";
    WriteSkill(stable / "skills", "alpha", SkillText("alpha", "stable user"));
    WriteSkill(other / "skills", "alpha", SkillText("alpha", "request user"));
    WriteSkill(stable / "extra", "extra", SkillText("extra", "stable extra"));
    WriteSkill(other / "extra", "extra", SkillText("extra", "request extra"));

    std::error_code cwd_error;
    const fs::path old_cwd = fs::current_path(cwd_error);
    if (cwd_error) {
        std::cout << "SKIP: stable root base test: " << cwd_error.message() << "\n";
        return;
    }
    fs::current_path(stable, cwd_error);
    if (cwd_error) {
        std::cout << "SKIP: stable root base test: " << cwd_error.message() << "\n";
        return;
    }

    {
        aiagent::SkillsConfig config;
        config.project = false;
        config.extra_roots = {"extra"};
        // 相对 user root 应固定在 loader 构造时的 cwd；extra root 仍跟请求 cwd。
        aiagent::skills::SkillLoader loader(config, {"skills"}, {});
        const auto catalog = loader.LoadCatalog(other.string());

        const auto* alpha = FindEntry(catalog, "alpha");
        Expect(alpha != nullptr && alpha->description == "stable user",
               "user/builtin relative roots must not follow the request cwd");
        const auto* extra = FindEntry(catalog, "extra");
        Expect(extra != nullptr && extra->description == "request extra",
               "extra_roots should still resolve relative to the request cwd");
    }

    std::error_code restore_error;
    fs::current_path(old_cwd, restore_error);
    Expect(!restore_error, "stable root base test should restore cwd");
}

}  // namespace

int main() {
    TestParseSkillsConfig();
    TestDiscoveryAndPrecedence();
    TestFrontmatterVariants();
    TestLimitsAndTruncation();
    TestCacheRefresh();
    TestResources();
    TestSymlinks();
    TestDuplicateWithinRoot();
    TestStableRootBase();

    if (g_failures == 0) {
        std::cout << "skill loader tests passed\n";
        return 0;
    }
    std::cerr << "skill loader tests failed with " << g_failures << " failure(s)\n";
    return 1;
}
