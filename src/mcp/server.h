#pragma once
#include <functional>
#include <iosfwd>
#include <map>
#include <string>
#include <nlohmann/json.hpp>

namespace et::mcp {

using json = nlohmann::json;

struct ToolError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Tool {
  std::string name;
  std::string description;
  json input_schema;
  std::function<json(const json& args)> handler;  // returns result payload; throws ToolError on failure
};

class Server {
 public:
  Server(std::string name, std::string version);
  void add_tool(Tool tool);

  // Handles one JSON-RPC message. Returns a response, or null json for notifications.
  json handle(const json& message);

  // Newline-delimited JSON-RPC over streams (MCP stdio transport).
  void serve(std::istream& in, std::ostream& out);

 private:
  json call_tool(const json& params);
  std::string name_, version_;
  std::map<std::string, Tool> tools_;
};

}  // namespace et::mcp
