#include "aiagent/tools/skill_tool.hpp"

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace aiagent::tools {
namespace {

std::string FormatResourceList(const std::vector<std::string>& resources) {
    if (resources.empty()) {
        return "(none)";
    }
    std::string text;
    for (const auto& resource : resources) {
        if (!text.empty()) {
            text += ", ";
        }
        text += resource;
    }
    return text;
}

std::string FormatBody(const skills::SkillContent& content) {
    std::ostringstream output;
    output << "Skill: " << content.entry.name << '\n';
    output << "Source: " << skills::SkillSourceName(content.entry.source) << '\n';
    output << "Path: " << content.path << '\n';
    output << "Files: " << FormatResourceList(content.resources) << "\n\n---\n";
    output << content.body;
    if (!content.body.empty() && content.body.back() != '\n') {
        output << '\n';
    }
    return output.str();
}

std::string FormatResource(const skills::SkillContent& content, const std::string& file) {
    std::ostringstream output;
    output << "Skill: " << content.entry.name << '\n';
    output << "Source: " << skills::SkillSourceName(content.entry.source) << '\n';
    output << "File: " << file << '\n';
    output << "Path: " << content.path << "\n\n---\n";
    output << content.body;
    if (!content.body.empty() && content.body.back() != '\n') {
        output << '\n';
    }
    return output.str();
}

ToolExecutionResult Failure(std::string error) {
    ToolExecutionResult result;
    result.ok = false;
    result.error = std::move(error);
    return result;
}

}  // namespace

SkillToolProvider::SkillToolProvider(std::shared_ptr<const skills::SkillLoader> loader,
                                     std::string fallback_working_directory)
    : loader_(std::move(loader)),
      fallback_working_directory_(std::move(fallback_working_directory)) {}

std::vector<llm::ToolSpec> SkillToolProvider::ListTools() {
    llm::ToolSpec spec;
    spec.name = kSkillToolName;
    spec.description =
            "Load the full instructions for a skill from the Skills catalog. Call this before "
            "starting a task that matches a skill description. Optionally read a file inside the "
            "skill directory with `file`.";
    spec.parameters = {
            {"type", "object"},
            {"properties",
             {{"name",
               {{"type", "string"}, {"description", "Exact skill name from the Skills catalog."}}},
              {"file",
               {{"type", "string"},
                {"description",
                 "Optional path relative to the skill directory, e.g. reference.md. Omit to "
                 "load SKILL.md."}}}}},
            {"required", nlohmann::json::array({"name"})}};
    return {spec};
}

ToolExecutionResult SkillToolProvider::Execute(const std::string& name,
                                               const nlohmann::json& arguments,
                                               const ToolContext& context) {
    if (name != kSkillToolName) {
        return Failure("unknown tool: " + name);
    }
    if (!arguments.is_object()) {
        return Failure("skill arguments must be an object");
    }
    if (!arguments.contains("name") || !arguments["name"].is_string()) {
        return Failure("skill requires a string `name`");
    }
    std::string skill_name = arguments["name"].get<std::string>();
    if (skill_name.empty()) {
        return Failure("skill `name` must not be empty");
    }

    std::string file;
    if (arguments.contains("file")) {
        if (!arguments["file"].is_string()) {
            return Failure("skill `file` must be a string");
        }
        file = arguments["file"].get<std::string>();
    }

    if (loader_ == nullptr) {
        return Failure("skill loader is not available");
    }
    const std::string working_directory = context.working_directory.empty()
                                                  ? fallback_working_directory_
                                                  : context.working_directory;
    skills::SkillContent content;
    std::string error;
    if (!loader_->LoadContent(skill_name, file, working_directory, &content, &error)) {
        return Failure(error);
    }

    ToolExecutionResult result;
    result.ok = true;
    result.text = file.empty() ? FormatBody(content) : FormatResource(content, file);
    return result;
}

std::shared_ptr<IToolProvider> MakeSkillToolProvider(
        std::shared_ptr<const skills::SkillLoader> loader, std::string fallback_working_directory) {
    return std::make_shared<SkillToolProvider>(std::move(loader),
                                               std::move(fallback_working_directory));
}

}  // namespace aiagent::tools
