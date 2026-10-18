#include "tools/timeline.h"

#include <filesystem>
#include <fstream>
#include <memory>

#include "model/timeline.h"
#include "render/render.h"
#include "tools/media.h"

namespace et::tools {

using mcp::json;
using mcp::Tool;

namespace {

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

json props(json p, std::vector<std::string> required = {}) {
  return {{"type", "object"}, {"properties", std::move(p)}, {"required", required}};
}
const json kStr = {{"type", "string"}};
const json kNum = {{"type", "number"}};
const json kInt = {{"type", "integer"}};

// Runs fn, translating model-level errors into tool errors so the model can recover.
template <class F>
json guarded(F&& fn) {
  try {
    return fn();
  } catch (const mcp::ToolError&) {
    throw;
  } catch (const std::exception& e) {
    throw mcp::ToolError(e.what());
  }
}

model::Asset asset_from_probe(const json& info) {
  model::Asset a;
  a.path = info["path"];
  a.duration = info["duration_sec"];
  for (auto& s : info["streams"]) {
    if (s["type"] == "video" && !a.has_video) {
      a.has_video = true;
      a.width = s["width"];
      a.height = s["height"];
    } else if (s["type"] == "audio") {
      a.has_audio = true;
    }
  }
  return a;
}

std::string absolute(const std::string& p) { return std::filesystem::absolute(p).lexically_normal().string(); }

}  // namespace

void register_timeline_tools(mcp::Server& server) {
  auto s = std::make_shared<Session>();

  server.add_tool({"create_project", "Create a new project and save it to project_path (a .json file). Replaces any open project.",
                   props({{"project_path", kStr}, {"name", kStr}, {"width", kInt}, {"height", kInt}, {"fps", kNum}},
                         {"project_path"}),
                   [s](const json& a) {
                     return guarded([&] {
                       auto p = std::make_unique<model::Project>();
                       p->name = a.value("name", "untitled");
                       p->width = a.value("width", 1920);
                       p->height = a.value("height", 1080);
                       p->fps = a.value("fps", 30.0);
                       if (p->width <= 0 || p->height <= 0 || p->fps <= 0) throw std::invalid_argument("invalid size/fps");
                       s->project = std::move(p);
                       s->path = absolute(a.at("project_path"));
                       s->save();
                       return s->project->to_json();
                     });
                   }});

  server.add_tool({"open_project", "Open an existing project.json.", props({{"project_path", kStr}}, {"project_path"}),
                   [s](const json& a) {
                     return guarded([&] {
                       std::ifstream f(absolute(a.at("project_path")));
                       if (!f) throw std::invalid_argument("cannot read project file");
                       s->project = std::make_unique<model::Project>(model::Project::from_json(json::parse(f)));
                       s->path = absolute(a.at("project_path"));
                       return s->project->to_json();
                     });
                   }});

  server.add_tool({"get_timeline", "Return the current project: assets, clips (with timeline start), duration.",
                   props({}), [s](const json&) { return guarded([&] { return s->get().to_json(); }); }});

  server.add_tool({"add_clip",
                   "Append (or insert at index) a clip from a media file. in/out are seconds in the source; "
                   "default is the whole file. Imports the file automatically.",
                   props({{"path", kStr}, {"in", kNum}, {"out", kNum}, {"index", kInt}}, {"path"}),
                   [s](const json& a) {
                     return guarded([&] {
                       auto& p = s->get();
                       auto asset = p.add_asset(asset_from_probe(probe_media(absolute(a.at("path")))));
                       auto in = a.value("in", 0.0);
                       auto out = a.value("out", asset.duration);
                       p.add_clip(asset.id, in, out, a.value("index", -1));
                       s->save();
                       return p.to_json();
                     });
                   }});

  server.add_tool({"trim_clip", "Change a clip's source in/out (seconds).",
                   props({{"clip_id", kStr}, {"in", kNum}, {"out", kNum}}, {"clip_id", "in", "out"}),
                   [s](const json& a) {
                     return guarded([&] {
                       s->get().trim_clip(a.at("clip_id"), a.at("in"), a.at("out"));
                       s->save();
                       return s->get().to_json();
                     });
                   }});

  server.add_tool({"move_clip", "Move a clip to a new position (0-based index) in the sequence.",
                   props({{"clip_id", kStr}, {"index", kInt}}, {"clip_id", "index"}),
                   [s](const json& a) {
                     return guarded([&] {
                       s->get().move_clip(a.at("clip_id"), a.at("index"));
                       s->save();
                       return s->get().to_json();
                     });
                   }});

  server.add_tool({"delete_clip", "Remove a clip; later clips close the gap.", props({{"clip_id", kStr}}, {"clip_id"}),
                   [s](const json& a) {
                     return guarded([&] {
                       s->get().delete_clip(a.at("clip_id"));
                       s->save();
                       return s->get().to_json();
                     });
                   }});

  for (bool preview : {false, true}) {
    server.add_tool({preview ? "render_preview" : "render_export",
                     preview ? "Fast low-quality render for checking the edit." : "Full-quality render to an .mp4 file.",
                     props({{"output_path", kStr}}, {"output_path"}),
                     [s, preview](const json& a) {
                       return guarded([&] {
                         auto out = absolute(a.at("output_path"));
                         render::render(s->get(), {out, preview});
                         auto info = probe_media(out);
                         return json{{"output", out}, {"duration_sec", info["duration_sec"]}};
                       });
                     }});
  }
}

}  // namespace et::tools
