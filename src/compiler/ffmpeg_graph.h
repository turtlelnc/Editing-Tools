#pragma once
#include <string>
#include <vector>

#include "model/timeline.h"

namespace et::compiler {

struct Options {
  std::string output;
  bool preview = false;  // fast, low-quality encode
  std::string ass_path;  // ASS file to burn in; required when the project has subtitles
};

// Builds the full ffmpeg argv (including "ffmpeg") for the project's timeline.
// Throws std::invalid_argument if the timeline is empty.
std::vector<std::string> compile(const model::Project& p, const Options& opt);

}  // namespace et::compiler
