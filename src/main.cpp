#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "a2a/core/version.hpp"
#include "a2a/models/agent_card.hpp"
#include "a2a/models/message.hpp"
#include "a2a/models/requests.hpp"
#include "a2a/models/task.hpp"
#include "a2a/server/agent_http_server.hpp"
#include "a2a/server/agent_server.hpp"
#include "a2a/server/sqlite_task_store.hpp"

#include "aiagent/agent/agent_runtime.hpp"
#include "aiagent/config.hpp"
#include "aiagent/llm/deepseek_client.hpp"
#include "aiagent/version.hpp"

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void OnSignal(int) {
    g_stop_requested = 1;
}

struct AppOptions {
    int port = 18080;
    std::string config_path = "conf/conf.ini";
    std::string db_path = "build/db/aiagent_tasks.db";
};

AppOptions ParseOptions(int argc, char** argv) {
    AppOptions options;
    if (const char* value = std::getenv("AIAGENT_PORT")) {
        options.port = std::stoi(value);
    }
    if (const char* value = std::getenv("AIAGENT_CONFIG")) {
        options.config_path = value;
    }
    if (const char* value = std::getenv("AIAGENT_DB")) {
        options.db_path = value;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            options.config_path = argv[++i];
        } else if (arg == "--db" && i + 1 < argc) {
            options.db_path = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            options.port = std::stoi(argv[++i]);
        } else {
            options.port = std::stoi(arg);
        }
    }
    return options;
}

a2a::models::AgentCard BuildAgentCard(int port) {
    using a2a::models::AgentCapabilities;
    using a2a::models::AgentCard;
    using a2a::models::AgentInterface;
    using a2a::models::AgentSkill;

    AgentInterface iface;
    iface.url = "http://127.0.0.1:" + std::to_string(port) + "/";
    iface.protocol_binding = std::string(a2a::models::kBindingJsonRpc);
    iface.protocol_version = std::string(a2a::core::kProtocolVersion);

    AgentCapabilities capabilities;
    capabilities.streaming = false;
    capabilities.push_notifications = false;

    AgentSkill skill;
    skill.id = "deepseek-chat";
    skill.name = "DeepSeek Chat";
    skill.description = "多轮对话，使用 DeepSeek 模型回复用户消息。";
    skill.tags = {"chat", "deepseek"};

    AgentCard card("ai-agent-cpp DeepSeek Agent",
                   "A2A agent backed by DeepSeek Chat Completions.",
                   aiagent::kVersion);
    card.WithInterface(std::move(iface))
            .WithCapabilities(std::move(capabilities))
            .WithDefaultInputModes({"text/plain"})
            .WithDefaultOutputModes({"text/plain"})
            .WithSkill(std::move(skill));
    return card;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const AppOptions options = ParseOptions(argc, argv);

        std::string config_error;
        const auto config = aiagent::LoadDeepSeekConfigFile(options.config_path, &config_error);
        if (!config.has_value()) {
            std::cerr << "配置错误: " << config_error << '\n';
            return 1;
        }

        const std::filesystem::path db_path(options.db_path);
        if (db_path.has_parent_path()) {
            std::filesystem::create_directories(db_path.parent_path());
        }

        auto task_store =
                std::make_shared<a2a::server::SqliteTaskStore>(db_path.string());
        auto model = std::make_shared<aiagent::llm::DeepSeekClient>(*config);
        aiagent::agent::AgentRuntime runtime(model);
        a2a::server::AgentServer agent_server(BuildAgentCard(options.port), task_store);

        agent_server.task_manager().SetMessageHandler(
                [&](const a2a::models::SendMessageRequest& request) {
                    std::vector<a2a::models::Message> history;
                    std::string context_id = request.message.context_id;
                    if (!context_id.empty()) {
                        std::size_t latest_history_size = 0;
                        for (const auto& task : task_store->ListAll()) {
                            if (task.context_id == context_id &&
                                task.history.size() > latest_history_size) {
                                history = task.history;
                                latest_history_size = task.history.size();
                            }
                        }
                    }
                    history.push_back(request.message);
                    return runtime.HandleMessage(request.message, history, context_id);
                });

        a2a::server::AgentHttpServer http_server(agent_server);
        if (!http_server.Start("127.0.0.1", options.port)) {
            std::cerr << "failed to start A2A HTTP server on port " << options.port << '\n';
            return 1;
        }

        std::signal(SIGINT, OnSignal);
        std::signal(SIGTERM, OnSignal);

        std::cout << "ai-agent-cpp listening on http://127.0.0.1:" << options.port << "/\n";
        std::cout << "Agent Card: http://127.0.0.1:" << options.port
                  << "/.well-known/agent-card.json\n";
        std::cout << "Model: " << config->model << '\n';
        std::cout << "Task DB: " << options.db_path << '\n';
        std::cout << "Press Ctrl+C to stop.\n";

        while (!g_stop_requested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        http_server.Stop();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "aiagent_app failed: " << e.what() << '\n';
        return 1;
    }
}
