#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "a2a/core/id.hpp"
#include "a2a/core/version.hpp"
#include "a2a/models/agent_card.hpp"
#include "a2a/models/message.hpp"
#include "a2a/models/operations.hpp"
#include "a2a/models/requests.hpp"
#include "a2a/models/task.hpp"
#include "a2a/server/agent_http_server.hpp"
#include "a2a/server/agent_server.hpp"

#include "aiagent/version.hpp"

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void OnSignal(int) {
  g_stop_requested = 1;
}

a2a::models::AgentCard BuildM0AgentCard() {
  using a2a::models::AgentCapabilities;
  using a2a::models::AgentCard;
  using a2a::models::AgentInterface;
  using a2a::models::AgentSkill;

  AgentInterface iface;
  iface.url = "http://127.0.0.1:18080/";
  iface.protocol_binding = std::string(a2a::models::kBindingJsonRpc);
  iface.protocol_version = std::string(a2a::core::kProtocolVersion);

  AgentCapabilities capabilities;
  capabilities.streaming = false;
  capabilities.push_notifications = false;

  AgentSkill skill;
  skill.id = "m0-echo";
  skill.name = "M0 Echo";
  skill.description = "M0 骨架：把收到的文本作为 artifact 原样返回。";
  skill.tags = {"m0", "echo"};

  AgentCard card("ai-agent-cpp M0",
                 "Minimal AI Agent skeleton built on a2a-protocol.",
                 aiagent::kVersion);
  card.WithInterface(std::move(iface))
      .WithCapabilities(std::move(capabilities))
      .WithDefaultInputModes({"text/plain"})
      .WithDefaultOutputModes({"text/plain"})
      .WithSkill(std::move(skill));
  return card;
}

std::string FirstText(const a2a::models::Message& message) {
  for (const auto& part : message.parts) {
    if (part.text.has_value()) {
      return *part.text;
    }
  }
  return {};
}

int ParsePort(int argc, char** argv) {
  int port = 18080;
  if (const char* env_port = std::getenv("AIAGENT_PORT")) {
    port = std::stoi(env_port);
  }
  if (argc > 1) {
    port = std::stoi(argv[1]);
  }
  return port;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const int port = ParsePort(argc, argv);
    const auto card = BuildM0AgentCard();

    a2a::server::AgentServer agent_server(card);

    // M0 最小业务回调：把收到的文本包进一个已完成 task 的 artifact。
    agent_server.task_manager().SetMessageHandler(
        [](const a2a::models::SendMessageRequest& request) {
          const std::string text = FirstText(request.message);

          a2a::models::Artifact artifact;
          artifact.artifact_id = "artifact-" + request.message.message_id;
          artifact.name = "echo";
          artifact.parts.push_back(a2a::models::Part::Text(text));

          a2a::models::Task task;
          task.WithState(a2a::models::TaskState::kCompleted)
              .WithArtifact(std::move(artifact));

          a2a::models::SendMessageResponse response;
          response.task = std::move(task);
          return response;
        });

    a2a::server::AgentHttpServer http_server(agent_server);
    if (!http_server.Start("127.0.0.1", port)) {
      std::cerr << "failed to start A2A HTTP server on port " << port << '\n';
      return 1;
    }

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    std::cout << "ai-agent-cpp M0 listening on http://127.0.0.1:" << port << "/\n";
    std::cout << "Agent Card: http://127.0.0.1:" << port
              << "/.well-known/agent-card.json\n";
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
