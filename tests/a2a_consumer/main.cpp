#include <a2a/core/json_rpc.hpp>
#include <a2a/models/agent_card.hpp>
#include <a2a/server/agent_server.hpp>

#include <iostream>

int main() {
    a2a::core::JsonRpcRequest request(1, "consumer.ping", {{"ok", true}});
    const auto wire = request.ToJson();
    const auto parsed = a2a::core::JsonRpcRequest::Parse(wire);
    if (parsed.method() != "consumer.ping") {
        std::cerr << "request roundtrip failed\n";
        return 1;
    }

    a2a::models::AgentCard card("consumer", "install/find_package smoke", "1.0.0");
    a2a::server::AgentServer server(card);
    server.RegisterMethod("consumer.ping",
                          [](const nlohmann::json& params) { return params; });

    const auto response = server.HandleRequest(parsed);
    if (!response || response->value("result", nlohmann::json{}) !=
                         nlohmann::json{{"ok", true}}) {
        std::cerr << "server response failed\n";
        return 2;
    }

    std::cout << "a2a consumer ok\n";
    return 0;
}
