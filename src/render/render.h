#pragma once
#include <string>

#include "compiler/ffmpeg_graph.h"

namespace et::render {

// Compiles and runs ffmpeg. Throws std::runtime_error with ffmpeg's stderr on failure,
// or with a clear message when a LUT file is missing or ffmpeg lacks libass.
void render(const model::Project& p, compiler::Options opt);

// Renders one frame of the timeline at `time` seconds and returns it as JPEG bytes.
std::string frame(const model::Project& p, double time, int max_width);

}  // namespace et::render
