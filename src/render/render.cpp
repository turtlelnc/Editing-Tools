#include "render/render.h"

#include <stdexcept>

#include "process.h"

namespace et::render {

void render(const model::Project& p, const compiler::Options& opt) {
  auto r = run(compiler::compile(p, opt));
  if (r.exit_code != 0) throw std::runtime_error("ffmpeg failed: " + r.err);
}

}  // namespace et::render
