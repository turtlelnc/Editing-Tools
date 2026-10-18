#include "mcp/server.h"

#include <iostream>

namespace et::mcp {

namespace {
json error_response(const json& id, int code, const std::string& msg) {
  return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", msg}}}};
}
}  // namespace

Server::Server(std::string name, std::string version)
    : name_(std::move(name)), version_(std::move(version)) {}

void Server::add_tool(Tool tool) {
  auto key = tool.name;
  tools_.emplace(std::move(key), std::move(tool));
}

json Server::call_tool(const json& params) {
  auto it = tools_.find(params.value("name", ""));
  if (it == tools_.end()) throw ToolError("unknown tool: " + params.value("name", ""));
  return it->second.handler(params.value("arguments", json::object()));
}

json Server::handle(const json& msg) {
  if (!msg.is_object() || !msg.contains("method")) {
    return error_response(msg.value("id", json(nullptr)), -32600, "invalid request");
  }
  const std::string method = msg["method"];
  const bool is_notification = !msg.contains("id");
  const json id = msg.value("id", json(nullptr));

  if (is_notification) return nullptr;  // e.g. notifications/initialized

  if (method == "initialize") {
    return {{"jsonrpc", "2.0"}, {"id", id},
            {"result", {{"protocolVersion", msg["params"].value("protocolVersion", "2025-06-18")},
                        {"capabilities", {{"tools", json::object()}}},
                        {"serverInfo", {{"name", name_}, {"version", version_}}}}}};
  }
  if (method == "ping") return {{"jsonrpc", "2.0"}, {"id", id}, {"result", json::object()}};

  if (method == "tools/list") {
    json list = json::array();
    for (auto& [_, t] : tools_)
      list.push_back({{"name", t.name}, {"description", t.description}, {"inputSchema", t.input_schema}});
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"tools", list}}}};
  }

  if (method == "tools/call") {
    try {
      json payload = call_tool(msg.value("params", json::object()));
      json content = json::array();
      // A handler may attach an image as {"__image": {"data": <base64>, "mime": "image/jpeg"}}.
      if (payload.is_object() && payload.contains("__image")) {
        json img = payload["__image"];
        payload.erase("__image");
        content.push_back({{"type", "image"}, {"data", img.value("data", "")}, {"mimeType", img.value("mime", "image/jpeg")}});
      }
      content.push_back({{"type", "text"}, {"text", payload.dump(2)}});
      return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"content", content}, {"isError", false}}}};
    } catch (const std::exception& e) {
      // Tool failures are reported in-band so the model can see and react to them.
      return {{"jsonrpc", "2.0"}, {"id", id},
              {"result", {{"content", {{{"type", "text"}, {"text", e.what()}}}}, {"isError", true}}}};
    }
  }
  return error_response(id, -32601, "method not found: " + method);
}

void Server::serve(std::istream& in, std::ostream& out) {
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    json resp;
    try {
      resp = handle(json::parse(line));
    } catch (const json::parse_error& e) {
      resp = error_response(nullptr, -32700, e.what());
    }
    if (!resp.is_null()) out << resp.dump() << '\n' << std::flush;
  }
}

}  // namespace et::mcp
