#pragma once
#include <string>
#include <vector>

#include "compiler/ffmpeg_graph.h"

namespace et::render {

// Compiles and runs ffmpeg. Throws std::runtime_error with ffmpeg's stderr on failure,
// or with a clear message when a LUT file is missing or ffmpeg lacks libass.
//
// When an activity hook is installed (see util/activity.h) progress lines are reported while ffmpeg runs and the
// render is killed as soon as the hook reports cancellation; the call then throws std::runtime_error("render
// cancelled"). Without a hook nothing changes.
void render(const model::Project& p, compiler::Options opt);

// Renders one frame of the timeline at `time` seconds and returns it as JPEG bytes.
std::string frame(const model::Project& p, double time, int max_width);

// Renders several frames of the timeline and tiles them into one JPEG, left to right, top to bottom.
// `max_width` is the width of each cell. Times must be inside the timeline; columns is clamped to [1, 4].
std::string contact_sheet(const model::Project& p, const std::vector<double>& times, int max_width, int columns);

// Tiles already-rendered JPEGs into one image (used for before/after comparisons). All images must share a size.
std::string tile_jpegs(const std::vector<std::string>& jpegs, int columns);

}  // namespace et::render
