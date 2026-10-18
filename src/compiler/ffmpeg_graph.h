#pragma once
#include <map>
#include <string>
#include <vector>

#include "model/timeline.h"

namespace et::compiler {

struct Options {
  std::string output;
  bool preview = false;  // fast, low-quality encode
  std::string ass_path;  // ASS file to burn in; required when the project has subtitles
  // LUT path as stored in the project -> a path safe to embed in a filtergraph (see render.cpp).
  // A LUT missing from this map must itself be made of filtergraph-safe characters.
  std::map<std::string, std::string> lut_paths;
  // >= 0 switches to single-frame mode: video only, writes one JPEG of the timeline at this time.
  double frame_time = -1;
  int frame_max_width = 0;  // downscale the frame to at most this width (0 = no scaling)
};

// Builds the full ffmpeg argv (including the ffmpeg binary) for the project's timeline.
// Throws std::invalid_argument if the timeline is empty or the options don't fit the project.
std::vector<std::string> compile(const model::Project& p, const Options& opt);

// Characters allowed in paths we embed inside a filtergraph without any escaping.
bool is_filter_safe_path(const std::string& path);

}  // namespace et::compiler
