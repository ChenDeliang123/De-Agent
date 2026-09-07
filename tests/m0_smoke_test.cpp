#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

#include "a2a/client/a2a_client.hpp"
#include "a2a/client/agent_card_resolver.hpp"
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

a2a::models::AgentCard BuildCard() {
  using a2a::models::AgentCapabilities;
  using a2a::models::AgentCard;
  using a2a::models::AgentInterface;

  AgentInterface iface;
  iface.url = "http://127.0.0.1/";
  iface.protocol_binding = std::string(a2a::models::kBindingJsonRpc);
  iface.protocol_version = std::string(a2a::core::kProtocolVersion);

  AgentCapabilities capabilities;
  capabilities.streaming = false;
  capabilities.push_notifications = false;

  AgentCard card("m0-smoke-agent",
                 "Smoke-test agent that echoes the first text part.",
                 aiagent::kVersion);
  card.WithInterface(std::move(iface)).WithCapabilities(std::move(capabilities));
  return card;
}

std::string FirstText(const a2a::models::Part& part) {
  return part.text.value_or("");
}

bool SendEchoRequest(const std::string& base_url) {
  a2a::client::A2AClient client(base_url);

  a2a::models::SendMessageRequest request;
  request.message = a2a::models::MakeTextMessage(
      a2a::models::Role::kUser, "hello-m0", "text/plain");

  const auto response = client.SendMessage(request);
  if (!response.IsTask()) {
    std::cerr << "expected a task-based response\n";
    return false;
  }

  const auto& task = response.GetTask();
  if (task.artifacts.empty() || task.artifacts.front().parts.empty()) {
    std::cerr << "expected the completed task to contain an artifact\n";
    return false;
  }

  const std::string echoed = FirstText(task.artifacts.front().parts.front());
  return echoed == "hello-m0";
}

}  // namespace

int main() {
  try {
    a2a::server::AgentServer agent_server(BuildCard());
    agent_server.task_manager().SetMessageHandler(
        [](const a2a::models::SendMessageRequest& request) {
          const std::string text = [](const a2a::models::Message& message) {
            for (const auto& part : message.parts) {
              if (part.text.has_value()) {
                return *part.text;
              }
            }
            return std::string();
          }(request.message);

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
    if (!http_server.Start("127.0.0.1", 0)) {
      std::cerr << "AgentHttpServer failed to start\n";
      return 1;
    }

    const std::string base_url =
        "http://127.0.0.1:" + std::to_string(http_server.bound_port());

    a2a::client::AgentCardResolver resolver(base_url);
    const auto card = resolver.Resolve();
    if (card.name() != "m0-smoke-agent") {
      std::cerr << "unexpected agent card name: " << card.name() << '\n';
      http_server.Stop();
      return 1;
    }

    if (!SendEchoRequest(base_url)) {
      http_server.Stop();
      return 1;
    }

    http_server.Stop();
    std::cout << "M0 smoke test passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "M0 smoke test failed: " << e.what() << '\n';
    return 1;
  }
}
