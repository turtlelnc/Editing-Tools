#include "tools/timeline.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>

#include "model/timeline.h"
#include "render/render.h"
#include "tools/media.h"
#include "tools/session.h"

namespace et::tools {

using mcp::Tool;

namespace {

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


}  // namespace

void register_timeline_tools(mcp::Server& server, std::shared_ptr<Session> s) {

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

  server.add_tool({"set_speed",
                   "Change a clip's playback speed: 2 = twice as fast (the clip becomes half as long on the timeline), "
                   "0.5 = half speed (slow motion, twice as long). Range 0.25-4. Changing a clip's length moves "
                   "everything after it, and subtitle/preview times on the output timeline shift with it, so re-check "
                   "with preview_frames afterwards.",
                   props({{"clip_id", kStr}, {"speed", kNum}}, {"clip_id", "speed"}),
                   [s](const json& a) {
                     return guarded([&] {
                       s->get().set_speed(a.at("clip_id"), a.at("speed"));
                       s->save();
                       return s->get().to_json();
                     });
                   }});

  server.add_tool({"add_music",
                   "Add background music (any file with audio) starting at `start` seconds on the timeline. "
                   "in/out select the source range (default: whole file). volume_db is gain (negative = quieter).",
                   props({{"path", kStr}, {"start", kNum}, {"in", kNum}, {"out", kNum}, {"volume_db", kNum}}, {"path"}),
                   [s](const json& a) {
                     return guarded([&] {
                       auto& p = s->get();
                       auto asset = p.add_asset(asset_from_probe(probe_media(absolute(a.at("path")))));
                       p.add_music(asset.id, a.value("in", 0.0), a.value("out", asset.duration), a.value("start", 0.0),
                                   a.value("volume_db", 0.0));
                       s->save();
                       return p.to_json();
                     });
                   }});

  server.add_tool({"update_music", "Change a music clip's source range or timeline start (seconds).",
                   props({{"music_id", kStr}, {"in", kNum}, {"out", kNum}, {"start", kNum}}, {"music_id"}),
                   [s](const json& a) {
                     return guarded([&] {
                       s->get().update_music(a.at("music_id"), opt<double>(a, "in"), opt<double>(a, "out"),
                                             opt<double>(a, "start"));
                       s->save();
                       return s->get().to_json();
                     });
                   }});

  server.add_tool({"delete_music", "Remove a music clip.", props({{"music_id", kStr}}, {"music_id"}),
                   [s](const json& a) {
                     return guarded([&] {
                       s->get().delete_music(a.at("music_id"));
                       s->save();
                       return s->get().to_json();
                     });
                   }});

  server.add_tool({"set_audio",
                   "Set sound properties of a video clip (id like c1) or music clip (id like m1): volume_db gain "
                   "[-60,24], fade_in/fade_out seconds, denoise on/off. Omitted fields are unchanged.",
                   props({{"id", kStr}, {"volume_db", kNum}, {"fade_in", kNum}, {"fade_out", kNum}, {"denoise", kBool}},
                         {"id"}),
                   [s](const json& a) {
                     return guarded([&] {
                       s->get().set_audio(a.at("id"), {opt<double>(a, "volume_db"), opt<double>(a, "fade_in"),
                                                       opt<double>(a, "fade_out"), opt<bool>(a, "denoise")});
                       s->save();
                       return s->get().to_json();
                     });
                   }});

  server.add_tool({"auto_duck",
                   "Automatically lower all music while the video clips' own audio (speech) is playing. "
                   "strength: light | medium | strong.",
                   props({{"enabled", kBool}, {"strength", kStr}}),
                   [s](const json& a) {
                     return guarded([&] {
                       s->get().set_duck(a.value("enabled", true), a.value("strength", "medium"));
                       s->save();
                       return s->get().to_json();
                     });
                   }});

  server.add_tool({"normalize_loudness",
                   "Normalize the final mix to a target integrated loudness (EBU R128). Default -16 LUFS "
                   "(online video), true peak -1.5 dB.",
                   props({{"enabled", kBool}, {"lufs", kNum}, {"true_peak", kNum}}),
                   [s](const json& a) {
                     return guarded([&] {
                       s->get().set_loudnorm(a.value("enabled", true), a.value("lufs", -16.0), a.value("true_peak", -1.5));
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
                         compiler::Options o;
                         o.output = out;
                         o.preview = preview;
                         render::render(s->get(), o);
                         auto info = probe_media(out);
                         return json{{"output", out}, {"duration_sec", info["duration_sec"]}};
                       });
                     }});
  }
}

}  // namespace et::tools
