#include "render/render.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "process.h"
#include "subtitles/ass.h"
#include "util/activity.h"

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

// Parses ffmpeg's `-progress pipe:1` key=value stream into a handful of readable progress lines.
class RenderProgress {
 public:
  RenderProgress(double total, std::string label) : total_(total), label_(std::move(label)) {}

  void feed(const char* data, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      if (data[i] == '\n') {
        handle(line_);
        line_.clear();
      } else if (line_.size() < 400) {
        line_ += data[i];
      }
    }
  }

  void finish() {
    if (!line_.empty()) handle(line_);
    if (last_percent_ < 100) {
      last_percent_ = 100;
      activity::report(label_ + " 100%");
    }
  }

 private:
  void handle(const std::string& line) {
    if (line.rfind("out_time_us=", 0) == 0) {
      long long micros = 0;
      try {
        micros = std::stoll(line.substr(12));
      } catch (...) {
        return;  // ffmpeg writes N/A for some frames
      }
      if (total_ <= 0) return;
      const double seconds = static_cast<double>(micros) / 1e6;
      const int percent = static_cast<int>(std::min(1.0, std::max(0.0, seconds / total_)) * 100);
      if (percent < last_percent_ + 5 && percent != 100) return;  // ~20 updates per render, not ~20 per second
      last_percent_ = percent;
      std::ostringstream out;
      out << label_ << " " << percent << "% (" << std::fixed << std::setprecision(1) << seconds << "s / " << total_
          << "s";
      if (!speed_.empty() && speed_ != "N/A") out << ", " << speed_;
      out << ")";
      activity::report(out.str());
    } else if (line.rfind("speed=", 0) == 0) {
      speed_ = line.substr(6);
    }
  }

  double total_ = 0;
  std::string label_;
  std::string line_;
  std::string speed_;
  int last_percent_ = -1;
};

std::string read_file(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream buf;
  buf << f.rdbuf();
  return buf.str();
}

}  // namespace

void render(const model::Project& p, compiler::Options opt) {
  if (activity::cancelled()) throw std::runtime_error("render cancelled");
  TempFiles temps;
  prepare(p, opt, temps);
  std::vector<std::string> argv = compiler::compile(p, opt);
  if (!activity::watching()) {
    auto r = run(argv);
    if (r.exit_code != 0) throw std::runtime_error("ffmpeg failed: " + r.err);
    return;
  }

  argv.insert(argv.begin() + 1, {"-progress", "pipe:1", "-nostats"});
  RenderProgress progress(p.duration(), opt.preview ? "rendering preview" : "rendering");
  StreamResult r = run_streaming(
      argv, [&progress](const char* data, size_t n) { progress.feed(data, n); return true; },
      [] { return activity::cancelled(); });
  if (r.cancelled) throw std::runtime_error("render cancelled");
  if (r.exit_code != 0) throw std::runtime_error("ffmpeg failed: " + r.err);
  progress.finish();
}

std::string frame(const model::Project& p, double time, int max_width) {
  if (activity::cancelled()) throw std::runtime_error("render cancelled");
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

std::string tile_jpegs(const std::vector<std::string>& jpegs, int columns) {
  if (jpegs.empty()) throw std::invalid_argument("nothing to tile");
  if (jpegs.size() == 1) return jpegs.front();
  const int cols = std::max(1, std::min(columns, 4));
  const size_t rows = (jpegs.size() + static_cast<size_t>(cols) - 1) / static_cast<size_t>(cols);

  TempFiles temps;
  std::vector<std::string> files;
  for (const auto& data : jpegs) {
    const fs::path path = temps.make(".jpg");
    std::ofstream(path, std::ios::binary) << data;
    files.push_back(path.string());
  }
  while (files.size() < rows * static_cast<size_t>(cols)) files.push_back(files.back());  // pad the last row

  std::vector<std::string> argv = {ffmpeg_bin(), "-y", "-v", "error"};
  for (const auto& file : files) argv.insert(argv.end(), {"-i", file});

  std::vector<std::string> parts;
  std::string last;
  for (size_t row = 0; row < rows; ++row) {
    std::string row_in;
    for (int col = 0; col < cols; ++col) row_in += "[" + std::to_string(row * static_cast<size_t>(cols) + static_cast<size_t>(col)) + "]";
    const std::string label = "row" + std::to_string(row);
    if (cols == 1) parts.push_back(row_in + "null[" + label + "]");  // one image per row
    else parts.push_back(row_in + "hstack=inputs=" + std::to_string(cols) + "[" + label + "]");
    last += "[" + label + "]";
  }
  std::string graph;
  std::string final_label;
  if (rows == 1) {
    graph = parts.front();
    final_label = "row0";
  } else {
    for (auto& part : parts) graph += (graph.empty() ? "" : ";") + part;
    graph += (graph.empty() ? "" : ";") + last + "vstack=inputs=" + std::to_string(rows) + "[sheet]";
    final_label = "sheet";
  }
  const fs::path output = temps.make(".jpg");
  argv.insert(argv.end(), {"-filter_complex", graph, "-map", "[" + final_label + "]", "-frames:v", "1", "-q:v", "3",
                           output.string()});
  auto r = run(argv);
  if (r.exit_code != 0) throw std::runtime_error("ffmpeg failed while tiling preview frames: " + r.err);
  const std::string bytes = read_file(output);
  if (bytes.empty()) throw std::runtime_error("ffmpeg produced an empty contact sheet");
  return bytes;
}

std::string contact_sheet(const model::Project& p, const std::vector<double>& times, int max_width, int columns) {
  if (times.empty()) throw std::invalid_argument("preview_frames needs at least one time");
  if (times.size() > 12) throw std::invalid_argument("preview_frames accepts at most 12 times");
  const double duration = p.duration();
  std::vector<std::string> jpegs;
  for (double t : times) {
    if (!(t >= 0 && t < duration))
      throw std::invalid_argument("time " + std::to_string(t) + " is outside the timeline [0, " + std::to_string(duration) +
                                  ")");
    jpegs.push_back(frame(p, t, max_width));
  }
  return tile_jpegs(jpegs, columns);
}

}  // namespace et::render
