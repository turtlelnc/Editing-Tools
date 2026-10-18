#pragma once
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "mcp/server.h"
#include "model/timeline.h"

namespace et::tools {

using mcp::json;

// One open project per server process, saved to its project.json after every mutation.
struct Session {
  std::unique_ptr<model::Project> project;
  std::string path;

  model::Project& get() {
    if (!project) throw mcp::ToolError("no project open: call create_project or open_project first");
    return *project;
  }
  void save() {
    if (path.empty()) return;
    std::ofstream(path) << project->to_json().dump(2);
  }
};

inline json props(json p, std::vector<std::string> required = {}) {
  return {{"type", "object"}, {"properties", std::move(p)}, {"required", required}};
}
inline const json kStr = {{"type", "string"}};
inline const json kNum = {{"type", "number"}};
inline const json kInt = {{"type", "integer"}};
inline const json kBool = {{"type", "boolean"}};

template <class T>
inline std::optional<T> opt(const json& a, const char* key) {
  if (!a.contains(key) || a[key].is_null()) return std::nullopt;
  return a[key].get<T>();
}

// Runs fn, translating model-level errors into tool errors so the model can recover.
template <class F>
inline json guarded(F&& fn) {
  try {
    return fn();
  } catch (const mcp::ToolError&) {
    throw;
  } catch (const std::exception& e) {
    throw mcp::ToolError(e.what());
  }
}

inline std::string absolute(const std::string& p) {
  return std::filesystem::absolute(p).lexically_normal().string();
}

}  // namespace et::tools
