#pragma once
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace et::agent {

using json = nlohmann::json;

struct Image {
  std::string mime = "image/jpeg";
  std::string data;  // base64
};

struct ToolCall {
  std::string id, name;
  json arguments = json::object();
};

// One provider-neutral conversation entry. Each provider translates these into its own wire format.
struct Message {
  enum class Role { System, User, Assistant, Tool };
  Role role = Role::User;
  std::string text;
  std::vector<Image> images;     // user attachments, or images produced by a tool
  std::vector<ToolCall> calls;   // assistant: tools it asked for
  std::string call_id;           // tool: which call this answers
  std::string tool_name;         // tool: name of the tool that ran
  bool is_error = false;         // tool: the call failed
};

struct ToolSpec {
  std::string name, description;
  json schema;  // JSON-Schema object for the arguments
};

}  // namespace et::agent
