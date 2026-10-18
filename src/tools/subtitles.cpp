#include "tools/subtitles.h"

#include <fstream>
#include <sstream>

#include "subtitles/ass.h"

namespace et::tools {

void register_subtitle_tools(mcp::Server& server, std::shared_ptr<Session> s) {
  const json cue_schema = {{"type", "object"},
                           {"properties", {{"start", kNum}, {"end", kNum}, {"text", kStr}}},
                           {"required", {"start", "end", "text"}}};

  server.add_tool({"set_subtitles",
                   "Replace ALL subtitles. Each cue has start/end in seconds on the OUTPUT timeline (after cuts) "
                   "and text (use \\n for a manual line break). Pass an empty list to remove subtitles. "
                   "Subtitles are burned into the video at render time.",
                   props({{"cues", {{"type", "array"}, {"items", cue_schema}}}}, {"cues"}),
                   [s](const json& a) {
                     return guarded([&] {
                       std::vector<model::SubtitleCue> cues;
                       for (auto& c : a.at("cues"))
                         cues.push_back({"", c.at("start").get<double>(), c.at("end").get<double>(), c.at("text")});
                       s->get().set_subtitles(std::move(cues));
                       s->save();
                       return s->get().to_json();
                     });
                   }});

  server.add_tool({"update_subtitle", "Change one subtitle's start, end (seconds) and/or text.",
                   props({{"id", kStr}, {"start", kNum}, {"end", kNum}, {"text", kStr}}, {"id"}),
                   [s](const json& a) {
                     return guarded([&] {
                       s->get().update_subtitle(a.at("id"), opt<double>(a, "start"), opt<double>(a, "end"),
                                                opt<std::string>(a, "text"));
                       s->save();
                       return s->get().to_json();
                     });
                   }});

  server.add_tool({"delete_subtitle", "Remove one subtitle by id (like s3).", props({{"id", kStr}}, {"id"}),
                   [s](const json& a) {
                     return guarded([&] {
                       s->get().delete_subtitle(a.at("id"));
                       s->save();
                       return s->get().to_json();
                     });
                   }});

  server.add_tool({"style_subtitles",
                   "Set how subtitles look; only the given fields change. color/outline_color are #RRGGBB. "
                   "position: bottom | middle | top. font_size is in pixels at the project's height (0 = auto). "
                   "box=true draws a translucent background box (box_opacity 0-1) instead of an outline. "
                   "font is an installed font family name; empty = a built-in CJK-capable default.",
                   props({{"font", kStr}, {"font_size", kNum}, {"color", kStr}, {"outline_color", kStr},
                          {"outline", kNum}, {"shadow", kNum}, {"bold", kBool}, {"position", kStr},
                          {"margin_v", kInt}, {"box", kBool}, {"box_opacity", kNum}}),
                   [s](const json& a) {
                     return guarded([&] {
                       s->get().set_subtitle_style(a);
                       s->save();
                       return s->get().to_json();
                     });
                   }});

  server.add_tool({"import_srt", "Load subtitles from an .srt file, replacing the current ones. offset (seconds) shifts all cues.",
                   props({{"path", kStr}, {"offset", kNum}}, {"path"}),
                   [s](const json& a) {
                     return guarded([&] {
                       std::ifstream f(absolute(a.at("path")));
                       if (!f) throw std::invalid_argument("cannot read SRT file");
                       std::stringstream buf;
                       buf << f.rdbuf();
                       auto cues = subtitles::parse_srt(buf.str());
                       double off = a.value("offset", 0.0);
                       for (auto& c : cues) { c.start += off; c.end += off; }
                       s->get().set_subtitles(std::move(cues));
                       s->save();
                       return s->get().to_json();
                     });
                   }});

  server.add_tool({"export_srt", "Write the current subtitles to an .srt file (for uploading to video platforms).",
                   props({{"output_path", kStr}}, {"output_path"}),
                   [s](const json& a) {
                     return guarded([&] {
                       auto& p = s->get();
                       if (p.subtitles.empty()) throw std::invalid_argument("there are no subtitles to export");
                       auto out = absolute(a.at("output_path"));
                       std::ofstream(out) << subtitles::to_srt(p.subtitles);
                       return json{{"output", out}, {"cues", p.subtitles.size()}};
                     });
                   }});
}

}  // namespace et::tools
