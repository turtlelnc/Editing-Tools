#include "agent/toolbox.h"

#include <algorithm>
#include <set>

#include "tools/all.h"
#include "util/base64.h"
#include "util/utf8.h"

namespace et::agent {

namespace fs = std::filesystem;
using mcp::ToolError;

namespace {

constexpr size_t kMaxResultChars = 12000;

void drop_defaults(json& obj, const json& defaults) {
  if (!obj.is_object()) return;
  for (auto it = obj.begin(); it != obj.end();) {
    if (defaults.contains(it.key()) && defaults[it.key()] == it.value()) it = obj.erase(it);
    else ++it;
  }
}

const json kAudioDefaults = {{"volume_db", 0.0}, {"fade_in", 0.0}, {"fade_out", 0.0}, {"denoise", false}};
const json kColorDefaults = {{"brightness", 0.0}, {"contrast", 1.0}, {"saturation", 1.0}, {"gamma", 1.0}, {"warmth", 0.0},
                             {"tint", 0.0}, {"curve_preset", ""}, {"curve_points", json::array()}, {"lut_path", ""},
                             {"lut_intensity", 1.0}};

std::string kind_of(const fs::path& p) {
  std::string e = p.extension().string();
  std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
  static const std::set<std::string> video = {".mp4", ".mov", ".mkv", ".avi", ".m4v", ".webm", ".mts", ".mxf"};
  static const std::set<std::string> audio = {".wav", ".mp3", ".m4a", ".aac", ".flac", ".ogg", ".aiff"};
  static const std::set<std::string> image = {".png", ".jpg", ".jpeg", ".heic", ".tif", ".tiff"};
  static const std::set<std::string> lut = {".cube", ".3dl", ".dat", ".m3d", ".csp"};
  static const std::set<std::string> sub = {".srt", ".ass", ".vtt"};
  if (video.count(e)) return "video";
  if (audio.count(e)) return "audio";
  if (image.count(e)) return "image";
  if (lut.count(e)) return "lut";
  if (sub.count(e)) return "subtitle";
  return "";
}

json obj_schema(json props, std::vector<std::string> required = {}) {
  return {{"type", "object"}, {"properties", std::move(props)}, {"required", required}};
}

}  // namespace

json compact_project(const json& p) {
  json out = json::object();
  for (const char* k : {"name", "width", "height", "fps", "duration"})
    if (p.contains(k)) out[k] = p[k];

  json assets = json::array();
  for (auto& a : p.value("assets", json::array()))
    assets.push_back({{"id", a["id"]}, {"path", a["path"]}, {"duration", a["duration"]}, {"audio", a.value("has_audio", false)}});
  out["assets"] = assets;

  json clips = json::array();
  for (auto& c : p.value("clips", json::array())) {
    json o = {{"id", c["id"]}, {"asset", c["asset"]}, {"in", c["in"]}, {"out", c["out"]}, {"start", c["start"]}};
    json audio = c.value("audio", json::object()), color = c.value("color", json::object());
    drop_defaults(audio, kAudioDefaults);
    drop_defaults(color, kColorDefaults);
    if (!audio.empty()) o["audio"] = audio;
    if (!color.empty()) o["color"] = color;
    clips.push_back(o);
  }
  out["clips"] = clips;

  json music = json::array();
  for (auto& m : p.value("music", json::array())) {
    json o = {{"id", m["id"]}, {"asset", m["asset"]}, {"in", m["in"]}, {"out", m["out"]}, {"start", m["start"]}};
    json audio = m.value("audio", json::object());
    drop_defaults(audio, kAudioDefaults);
    if (!audio.empty()) o["audio"] = audio;
    music.push_back(o);
  }
  if (!music.empty()) out["music"] = music;

  json color = p.value("color", json::object());
  drop_defaults(color, kColorDefaults);
  if (!color.empty()) out["color"] = color;

  if (p.value("duck", json::object()).value("enabled", false)) out["duck"] = p["duck"];
  if (p.value("loudnorm", json::object()).value("enabled", false)) out["loudnorm"] = p["loudnorm"];

  const json subs = p.value("subtitles", json::array());
  if (!subs.empty()) {
    json list = json::array();
    for (size_t i = 0; i < subs.size() && i < 60; ++i) list.push_back(subs[i]);
    out["subtitles"] = list;
    if (subs.size() > 60) out["subtitles_total"] = subs.size();
    out["subtitle_style"] = p["subtitle_style"];
  }
  return out;
}

ToolBox::ToolBox(std::shared_ptr<tools::Session> session, Policy policy)
    : session_(std::move(session)), policy_(std::move(policy)), server_("agent", "0") {
  tools::register_all_tools(server_, session_);

  server_.add_tool({"list_dir", "List the files and folders in a directory (read-only). Relative paths are inside the workspace.",
                    obj_schema({{"path", {{"type", "string"}}}}, {"path"}), [this](const json& a) {
                      const fs::path dir = policy_.resolve(a.at("path"));
                      std::error_code ec;
                      if (!fs::is_directory(dir, ec)) throw ToolError("not a directory: " + dir.string());
                      std::vector<json> entries;
                      for (auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
                        const std::string name = e.path().filename().string();
                        if (name.empty() || name[0] == '.') continue;
                        const bool is_dir = e.is_directory(ec);
                        json o = {{"name", name}, {"type", is_dir ? "dir" : "file"}};
                        if (!is_dir) {
                          o["size_mb"] = static_cast<double>(e.file_size(ec)) / 1048576.0;
                          if (auto k = kind_of(e.path()); !k.empty()) o["kind"] = k;
                        }
                        entries.push_back(o);
                      }
                      std::sort(entries.begin(), entries.end(), [](const json& x, const json& y) { return x["name"] < y["name"]; });
                      const size_t total = entries.size();
                      if (entries.size() > 200) entries.resize(200);
                      return json{{"path", dir.string()}, {"entries", entries}, {"total", total}};
                    }});

  server_.add_tool({"find_media",
                    "Search a folder (recursively, up to 6 levels) for media files: kinds video, audio, image, lut, subtitle. "
                    "Read-only. Returns at most `max_results` paths.",
                    obj_schema({{"root", {{"type", "string"}}},
                                {"kinds", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                                {"max_results", {{"type", "integer"}}}},
                               {"root"}),
                    [this](const json& a) {
                      const fs::path root = policy_.resolve(a.at("root"));
                      std::error_code ec;
                      if (!fs::is_directory(root, ec)) throw ToolError("not a directory: " + root.string());
                      std::set<std::string> kinds;
                      for (auto& k : a.value("kinds", json::array())) kinds.insert(k.get<std::string>());
                      const size_t limit = static_cast<size_t>(std::clamp(a.value("max_results", 100), 1, 500));
                      json files = json::array();
                      bool truncated = false;
                      fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
                      for (; it != end && !ec; it.increment(ec)) {
                        const std::string name = it->path().filename().string();
                        if (!name.empty() && name[0] == '.') {
                          if (it->is_directory(ec)) it.disable_recursion_pending();
                          continue;
                        }
                        if (it.depth() >= 6 && it->is_directory(ec)) { it.disable_recursion_pending(); continue; }
                        if (!it->is_regular_file(ec)) continue;
                        const std::string k = kind_of(it->path());
                        if (k.empty() || (!kinds.empty() && !kinds.count(k))) continue;
                        if (files.size() >= limit) { truncated = true; break; }
                        files.push_back({{"path", it->path().string()}, {"kind", k},
                                         {"size_mb", static_cast<double>(it->file_size(ec)) / 1048576.0}});
                      }
                      return json{{"root", root.string()}, {"files", files}, {"truncated", truncated}};
                    }});
}

std::vector<ToolSpec> ToolBox::specs() const {
  std::vector<ToolSpec> out;
  for (auto* t : server_.tools()) out.push_back({t->name, t->description, t->input_schema});
  return out;
}

ToolResult ToolBox::execute(const ToolCall& call) {
  try {
    return run(call);
  } catch (const std::exception& e) {
    return {e.what(), {}, true};
  }
}

ToolResult ToolBox::run(const ToolCall& call) {
  const mcp::Tool* tool = server_.find(call.name);
  if (!tool) throw ToolError("unknown tool '" + call.name + "'");
  if (call.arguments.contains("__invalid_arguments"))
    throw ToolError("the arguments were not valid JSON: " + call.arguments["__invalid_arguments"].get<std::string>());

  json args = call.arguments;
  for (auto& r : tool->input_schema["required"])
    if (!args.contains(r.get<std::string>())) throw ToolError("missing required argument: " + r.get<std::string>());
  if (call.name == "preview_frame" && !args.contains("max_width")) args["max_width"] = 768;  // keep model input small
  if (call.name == "preview_frames" && !args.contains("max_width")) args["max_width"] = 480;
  if (call.name == "preview_compare" && !args.contains("max_width")) args["max_width"] = 640;
  args = policy_.check(call.name, args);

  if (approver_ && call.name == "render_export") {
    std::string reason;
    if (!approver_(call.name, args, reason))
      throw ToolError(reason.empty() ? "the user declined this render" : reason);
  }

  json payload = server_.call(call.name, args);

  ToolResult result;
  if (payload.is_object() && payload.contains("__image")) {
    result.images.push_back({payload["__image"].value("mime", "image/jpeg"), payload["__image"].value("data", "")});
    payload.erase("__image");
  }
  if (payload.is_object() && payload.contains("clips") && payload.contains("assets") && payload.contains("duration"))
    payload = compact_project(payload);
  result.text = util::dump_json(payload);
  if (result.text.size() > kMaxResultChars)
    result.text = util::truncate_utf8(result.text, kMaxResultChars) + "…[truncated " +
                  std::to_string(result.text.size() - kMaxResultChars) + " chars]";
  return result;
}

}  // namespace et::agent
