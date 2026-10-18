#include "render/render.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <unistd.h>

#include "process.h"
#include "subtitles/ass.h"

namespace et::render {

namespace {
// Deletes the temporary ASS file when rendering ends, however it ends.
struct TempFile {
  std::string path;
  ~TempFile() {
    if (!path.empty()) std::filesystem::remove(path);
  }
};
}  // namespace

void render(const model::Project& p, const compiler::Options& opt) {
  compiler::Options o = opt;
  TempFile ass;
  if (!p.subtitles.empty()) {
    if (!ffmpeg_has_filter("ass"))
      throw std::runtime_error(
          "this ffmpeg (" + ffmpeg_bin() + ") has no libass 'ass' filter, which burning subtitles needs. "
          "Point EDITING_TOOLS_FFMPEG at an ffmpeg built with --enable-libass.");
    static std::atomic<int> counter{0};
    ass.path = (std::filesystem::temp_directory_path() /
                ("et_" + std::to_string(getpid()) + "_" + std::to_string(counter++) + ".ass")).string();
    std::ofstream(ass.path) << subtitles::to_ass(p);
    o.ass_path = ass.path;
  }
  auto r = run(compiler::compile(p, o));
  if (r.exit_code != 0) throw std::runtime_error("ffmpeg failed: " + r.err);
}

}  // namespace et::render
