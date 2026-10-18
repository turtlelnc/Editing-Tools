#include "render/render.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

#include "process.h"
#include "subtitles/ass.h"

namespace et::render {

namespace fs = std::filesystem;

namespace {

// Removes everything registered with it when rendering ends, however it ends.
struct TempFiles {
  std::vector<fs::path> paths;
  ~TempFiles() {
    std::error_code ec;
    for (auto& p : paths) fs::remove(p, ec);
  }
  fs::path make(const std::string& ext) {
    static std::atomic<int> counter{0};
    fs::path p = fs::temp_directory_path() / ("et_" + std::to_string(getpid()) + "_" + std::to_string(counter++) + ext);
    paths.push_back(p);
    return p;
  }
};

std::string lower_ext(const std::string& path) {
  std::string ext = fs::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
  return ext;
}

// Fills in the ASS file and LUT aliases that the compiler needs.
void prepare(const model::Project& p, compiler::Options& o, TempFiles& temps) {
  if (!p.subtitles.empty()) {
    if (!ffmpeg_has_filter("ass"))
      throw std::runtime_error(
          "this ffmpeg (" + ffmpeg_bin() + ") has no libass 'ass' filter, which burning subtitles needs. "
          "Point EDITING_TOOLS_FFMPEG at an ffmpeg built with --enable-libass.");
    auto path = temps.make(".ass");
    std::ofstream(path) << subtitles::to_ass(p);
    o.ass_path = path.string();
  }

  // LUTs often live on external drives with spaces or non-ASCII in the path, which the filtergraph syntax
  // cannot express safely. Link each one under a plain temporary name (ffmpeg picks the parser by extension).
  std::set<std::string> luts;
  if (!p.grade.lut_path.empty()) luts.insert(p.grade.lut_path);
  for (auto& c : p.clips)
    if (!c.grade.lut_path.empty()) luts.insert(c.grade.lut_path);
  for (auto& lut : luts) {
    std::error_code ec;
    if (!fs::exists(lut, ec))
      throw std::runtime_error("LUT file not found: " + lut + " (is the drive it lives on connected?)");
    if (compiler::is_filter_safe_path(lut) && fs::path(lut).is_absolute()) {
      o.lut_paths[lut] = lut;
      continue;
    }
    auto alias = temps.make(lower_ext(lut));
    fs::create_symlink(fs::absolute(lut), alias, ec);
    if (ec) {  // e.g. a filesystem without symlinks: fall back to a copy
      fs::copy_file(lut, alias, fs::copy_options::overwrite_existing, ec);
      if (ec) throw std::runtime_error("cannot read LUT file " + lut + ": " + ec.message());
    }
    o.lut_paths[lut] = alias.string();
  }
}

}  // namespace

void render(const model::Project& p, compiler::Options opt) {
  TempFiles temps;
  prepare(p, opt, temps);
  auto r = run(compiler::compile(p, opt));
  if (r.exit_code != 0) throw std::runtime_error("ffmpeg failed: " + r.err);
}

std::string frame(const model::Project& p, double time, int max_width) {
  TempFiles temps;
  compiler::Options opt;
  opt.output = temps.make(".jpg").string();
  opt.frame_time = time;
  opt.frame_max_width = max_width;
  prepare(p, opt, temps);
  auto r = run(compiler::compile(p, opt));
  if (r.exit_code != 0) throw std::runtime_error("ffmpeg failed: " + r.err);
  std::ifstream f(opt.output, std::ios::binary);
  std::stringstream buf;
  buf << f.rdbuf();
  if (buf.str().empty()) throw std::runtime_error("ffmpeg produced no frame at " + std::to_string(time) + "s");
  return buf.str();
}

}  // namespace et::render
