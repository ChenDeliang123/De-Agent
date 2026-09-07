#include <json.hpp>

#include <iostream>
#include <string>

int main() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        try {
            const nlohmann::json request = nlohmann::json::parse(line);
            const std::string method = request.value("method", std::string());
            nlohmann::json response;
            response["jsonrpc"] = "2.0";
            if (request.contains("id")) {
                response["id"] = request["id"];
            }

            if (method == "initialize") {
                response["result"] = {
                        {"protocolVersion", "2025-03-26"},
                        {"capabilities", {{"tools", nlohmann::json::object()}}},
                        {"serverInfo", {{"name", "fake-mcp"}, {"version", "test"}}}};
            } else if (method == "tools/list") {
                response["result"]["tools"] = nlohmann::json::array(
                        {{{"name", "calculate"},
                          {"description", "Calculate an arithmetic expression"},
                          {"inputSchema",
                           {{"type", "object"},
                            {"properties", {{"expr", {{"type", "string"}}}}}}}}});
            } else if (method == "tools/call") {
                response["result"] = {
                        {"content",
                         nlohmann::json::array({{{"type", "text"}, {"text", "4"}}})},
                        {"isError", false}};
            } else {
                response["error"] = {{"code", -32601}, {"message", "Method not found"}};
            }
            std::cout << response.dump() << '\n' << std::flush;
        } catch (const std::exception&) {
            std::cerr << "fake_mcp_server received invalid JSON\n";
        }
    }
    return 0;
}
