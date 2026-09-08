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
                // 两个工具：calculate 用于常规调用，bigtext 返回大文本，
                // 用于验证客户端缓冲读能跨多个 read 块正确拼接。
                response["result"]["tools"] = nlohmann::json::array(
                        {{{"name", "calculate"},
                          {"description", "Calculate an arithmetic expression"},
                          {"inputSchema",
                           {{"type", "object"},
                            {"properties", {{"expr", {{"type", "string"}}}}}}}},
                         {{"name", "bigtext"},
                          {"description", "Return a large text payload"},
                          {"inputSchema", {{"type", "object"}}}}});
            } else if (method == "tools/call") {
                const std::string tool_name =
                        request["params"].value("name", std::string());
                if (tool_name == "bigtext") {
                    const std::string big(200 * 1024, 'x');
                    response["result"] = {
                            {"content",
                             nlohmann::json::array({{{"type", "text"}, {"text", big}}})},
                            {"isError", false}};
                } else {
                    response["result"] = {
                            {"content",
                             nlohmann::json::array({{{"type", "text"}, {"text", "4"}}})},
                            {"isError", false}};
                }
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
