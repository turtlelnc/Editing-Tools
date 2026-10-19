#pragma once
#include <filesystem>
#include <fstream>
#include <map>
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
  // Whisper results keyed by "<media path>|<language>" so transcribe and auto_subtitles do not run the model twice.
  std::map<std::string, std::string> transcripts;

  model::Project& get() {
    if (!project) throw mcp::ToolError("no project open: call create_project or open_project first");
    return *project;
  }
  // Loads a project file (used by the CLI, where every invocation is a fresh process).
  void open(const std::string& file) {
    std::ifstream f(file);
    if (!f) throw mcp::ToolError("project file not found: " + file + " (create it with create_project)");
    project = std::make_unique<model::Project>(model::Project::from_json(json::parse(f)));
    path = std::filesystem::absolute(file).lexically_normal().string();
  }
  // Written via a temp file + rename so a crash or a concurrent reader never sees half a project.
  void save() {
    if (path.empty()) return;
    const std::string tmp = path + ".tmp";
    {
      std::ofstream f(tmp);
      f << project->to_json().dump(2);
      if (!f) throw mcp::ToolError("cannot write project file: " + path);
    }
    std::filesystem::rename(tmp, path);
  }
};

// JSON-Schema for a tool's arguments. Note `props({})` passes JSON null, not {}: normalize it, because
// MCP clients reject a null "properties".
inline json props(json p, std::vector<std::string> required = {}) {
  if (!p.is_object()) p = json::object();
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
