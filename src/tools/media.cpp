#include "tools/media.h"

#include <filesystem>

#include "process.h"

namespace et::tools {

using mcp::json;

json probe_media(const std::string& path) {
  if (!std::filesystem::exists(path)) throw mcp::ToolError("file not found: " + path);
  auto r = run({"ffprobe", "-v", "error", "-print_format", "json", "-show_format", "-show_streams", path});
  if (r.exit_code != 0) throw mcp::ToolError("ffprobe failed: " + r.err);

  json raw = json::parse(r.out);
  json streams = json::array();
  for (auto& s : raw.value("streams", json::array())) {
    json o = {{"index", s["index"]}, {"type", s.value("codec_type", "")}, {"codec", s.value("codec_name", "")}};
    if (s.value("codec_type", "") == "video") {
      o["width"] = s.value("width", 0);
      o["height"] = s.value("height", 0);
      o["frame_rate"] = s.value("avg_frame_rate", "");
    } else if (s.value("codec_type", "") == "audio") {
      o["sample_rate"] = s.value("sample_rate", "");
      o["channels"] = s.value("channels", 0);
    }
    streams.push_back(o);
  }
  auto& fmt = raw["format"];
  double duration = 0;
  if (fmt.contains("duration")) duration = std::stod(fmt["duration"].get<std::string>());
  return {{"path", path}, {"duration_sec", duration}, {"format", fmt.value("format_name", "")}, {"streams", streams}};
}

void register_media_tools(mcp::Server& server) {
  server.add_tool({"probe_media",
                   "Inspect a media file: duration, container, and video/audio stream details.",
                   {{"type", "object"},
                    {"properties", {{"path", {{"type", "string"}, {"description", "Absolute path to a media file"}}}}},
                    {"required", {"path"}}},
                   [](const json& args) { return probe_media(args.at("path").get<std::string>()); }});
}

}  // namespace et::tools
