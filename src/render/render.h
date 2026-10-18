#pragma once
#include <string>

#include "compiler/ffmpeg_graph.h"

namespace et::render {

// Compiles and runs ffmpeg. Throws std::runtime_error with ffmpeg's stderr on failure.
void render(const model::Project& p, const compiler::Options& opt);

}  // namespace et::render
