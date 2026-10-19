#include "tools/color.h"

#include <vector>

#include "render/render.h"
#include "util/base64.h"

namespace et::tools {

namespace {
const char* kTarget = "Clip id (like c1) to grade just that clip; omit to grade the whole project (applies to every clip).";
}

void register_color_tools(mcp::Server& server, std::shared_ptr<Session> s) {
  const json clip = {{"type", "string"}, {"description", kTarget}};

  server.add_tool({"color_adjust",
                   "Primary color correction. Only the given fields change. brightness [-1,1] (0 = unchanged), "
                   "contrast [0,3] (1 = unchanged), saturation [0,3] (0 = black & white, 1 = unchanged), "
                   "gamma [0.1,5] (>1 brightens midtones, 1 = unchanged), warmth [-1,1] (positive = warmer/orange, "
                   "negative = cooler/blue), tint [-1,1] (positive = magenta, negative = green). "
                   "Use preview_frame afterwards to check the look.",
                   props({{"clip_id", clip}, {"brightness", kNum}, {"contrast", kNum}, {"saturation", kNum},
                          {"gamma", kNum}, {"warmth", kNum}, {"tint", kNum}}),
                   [s](const json& a) {
                     return guarded([&] {
                       s->get().set_color(a.value("clip_id", ""), a);
                       s->save();
                       return s->get().to_json();
                     });
                   }});

  server.add_tool({"color_curves",
                   "Set a tone curve. Either preset (color_negative, cross_process, darker, increase_contrast, lighter, "
                   "linear_contrast, medium_contrast, negative, strong_contrast, vintage) or points: a list of [input, "
                   "output] pairs in [0,1] with increasing input, e.g. [[0,0],[0.25,0.18],[0.75,0.85],[1,1]] for an "
                   "S-curve. Give neither to clear the curve.",
                   props({{"clip_id", clip},
                          {"preset", kStr},
                          {"points", {{"type", "array"}, {"items", {{"type", "array"}, {"items", kNum}, {"minItems", 2}, {"maxItems", 2}}}}}}),
                   [s](const json& a) {
                     return guarded([&] {
                       std::vector<std::pair<double, double>> pts;
                       for (auto& p : a.value("points", json::array())) {
                         if (!p.is_array() || p.size() != 2) throw std::invalid_argument("each point must be [input, output]");
                         pts.emplace_back(p[0].get<double>(), p[1].get<double>());
                       }
                       s->get().set_curve(a.value("clip_id", ""), a.value("preset", ""), pts);
                       s->save();
                       return s->get().to_json();
                     });
                   }});

  server.add_tool({"apply_lut",
                   "Apply a 3D LUT (.cube, .3dl, .dat, .m3d, .csp) as a look. The file can be anywhere, including an "
                   "external drive (it must be connected when rendering). intensity 0-1 blends between the original "
                   "(0) and the full LUT (1, default).",
                   props({{"clip_id", clip}, {"path", kStr}, {"intensity", kNum}}, {"path"}),
                   [s](const json& a) {
                     return guarded([&] {
                       auto path = absolute(a.at("path"));
                       if (!std::filesystem::exists(path))
                         throw std::invalid_argument("LUT file not found: " + path + " (is the drive it lives on connected?)");
                       s->get().set_lut(a.value("clip_id", ""), path, a.value("intensity", 1.0));
                       s->save();
                       return s->get().to_json();
                     });
                   }});

  server.add_tool({"apply_style",
                   "Apply a ready-made look in one call, on a clip or the whole project. Presets: warm_cinematic, "
                   "cool_teal, faded_film, vivid, black_and_white, none (resets color). Use preview_compare to judge "
                   "it, then color_adjust for fine tuning.",
                   props({{"preset", kStr}, {"clip_id", clip}}, {"preset"}),
                   [s](const json& a) {
                     return guarded([&] {
                       auto& p = s->get();
                       const std::string preset = a.at("preset").get<std::string>();
                       const std::string clip_id = a.value("clip_id", "");
                       if (preset == "none") {
                         p.reset_color(clip_id);
                       } else {
                         json patch;
                         if (preset == "warm_cinematic")
                           patch = {{"warmth", 0.35}, {"contrast", 1.12}, {"saturation", 1.08}, {"brightness", 0.02}, {"tint", 0.03}};
                         else if (preset == "cool_teal")
                           patch = {{"warmth", -0.3}, {"tint", -0.05}, {"contrast", 1.1}, {"saturation", 0.95}};
                         else if (preset == "faded_film")
                           patch = {{"warmth", 0.05}, {"contrast", 0.85}, {"saturation", 0.82}, {"brightness", 0.06}, {"gamma", 1.12}};
                         else if (preset == "vivid")
                           patch = {{"warmth", 0.05}, {"contrast", 1.15}, {"saturation", 1.35}};
                         else if (preset == "black_and_white")
                           patch = {{"saturation", 0.0}, {"contrast", 1.2}};
                         else
                           throw std::invalid_argument(
                               "unknown preset '" + preset +
                               "' (warm_cinematic, cool_teal, faded_film, vivid, black_and_white, none)");
                         p.set_color(clip_id, patch);
                       }
                       s->save();
                       return p.to_json();
                     });
                   }});

  server.add_tool({"reset_color", "Remove all color work (adjustments, curve and LUT) from a clip, or from the whole project.",
                   props({{"clip_id", clip}}),
                   [s](const json& a) {
                     return guarded([&] {
                       s->get().reset_color(a.value("clip_id", ""));
                       s->save();
                       return s->get().to_json();
                     });
                   }});

  server.add_tool({"preview_frame",
                   "Render ONE frame of the current edit (with color, subtitles, cuts) at `time` seconds on the output "
                   "timeline and return it as an image, so you can judge the result. max_width defaults to 960.",
                   props({{"time", kNum}, {"max_width", kInt}}, {"time"}),
                   [s](const json& a) {
                     return guarded([&] {
                       auto& p = s->get();
                       const double t = a.at("time").get<double>();
                       if (t < 0 || t >= p.duration())
                         throw std::invalid_argument("time must be within [0, " + std::to_string(p.duration()) + ")");
                       auto jpeg = render::frame(p, t, a.value("max_width", 960));
                       return json{{"__image", {{"data", util::base64(jpeg)}, {"mime", "image/jpeg"}}},
                                   {"time", t}, {"timeline_duration", p.duration()}};
                     });
                   }});

  server.add_tool({"preview_frames",
                   "Render several frames of the output timeline and tile them into ONE contact-sheet image (reads left "
                   "to right, top to bottom). Use it to judge pacing, cut points and grade consistency across the edit. "
                   "`times` are seconds on the output timeline, at most 12. max_width is per frame (default 480), "
                   "columns defaults to 3.",
                   props({{"times", {{"type", "array"}, {"items", kNum}}}, {"max_width", kInt}, {"columns", kInt}},
                         {"times"}),
                   [s](const json& a) {
                     return guarded([&] {
                       auto& p = s->get();
                       std::vector<double> times;
                       for (const auto& v : a.at("times")) times.push_back(v.get<double>());
                       const auto jpeg = render::contact_sheet(p, times, a.value("max_width", 480), a.value("columns", 3));
                       return json{{"__image", {{"data", util::base64(jpeg)}, {"mime", "image/jpeg"}}},
                                   {"times", times}, {"timeline_duration", p.duration()}};
                     });
                   }});

  server.add_tool({"preview_compare",
                   "Before/after comparison of the color grade at `time`: the left half is the timeline WITHOUT any "
                   "grading, the right half is the current grade (same frame, cuts and subtitles). Use it to judge "
                   "whether a grade went too far. max_width defaults to 960 per side.",
                   props({{"time", kNum}, {"max_width", kInt}}, {"time"}),
                   [s](const json& a) {
                     return guarded([&] {
                       auto& p = s->get();
                       const double t = a.at("time").get<double>();
                       if (t < 0 || t >= p.duration())
                         throw std::invalid_argument("time must be within [0, " + std::to_string(p.duration()) + ")");
                       const int width = a.value("max_width", 960);
                       model::Project plain = p;
                       plain.grade = model::ColorGrade{};
                       for (auto& clip : plain.clips) clip.grade = model::ColorGrade{};
                       const auto original = render::frame(plain, t, width);
                       const auto graded = render::frame(p, t, width);
                       const auto tiled = render::tile_jpegs({original, graded}, 2);
                       return json{{"__image", {{"data", util::base64(tiled)}, {"mime", "image/jpeg"}}},
                                   {"time", t},
                                   {"timeline_duration", p.duration()},
                                   {"layout", "left = no color grade, right = current grade"}};
                     });
                   }});
}

}  // namespace et::tools
