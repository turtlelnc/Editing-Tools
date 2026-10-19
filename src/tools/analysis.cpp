// Analysis tools: scene changes, silence, and rough cuts.
//
// Everything here reads the source with ffmpeg/ffprobe first and only then touches the project, so a bad
// analysis never leaves a half-edited timeline behind.
//
// Note on the project side of auto_rough_cut: register_analysis_tools() only gets the Server (the session
// belongs to the timeline tools), so the mutation is expressed through the tools that already own it:
// create_project and add_clip. add_clip imports the asset for us and add_asset dedupes it by path, so a
// source is only ever represented once in the project. The handler therefore keeps a reference to the
// Server it was registered on, which means that Server must not be moved or copied afterwards.
#include "tools/analysis.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "process.h"
#include "tools/media.h"
#include "tools/session.h"

namespace et::tools {

using mcp::json;

namespace {

// Scene changes: the tools take a 0-100 percentage, ffmpeg's scene score is 0-1.
constexpr double kDefaultSceneThreshold = 8.0;
constexpr double kMinSceneGap = 0.1;  // seconds; closer changes are the same cut
constexpr double kEps = 1e-9;

struct Span {
  double start = 0, end = 0;
  double length() const { return end - start; }
};

struct SceneChange {
  double time = 0;
  std::optional<double> score;
};

// ---- small text/number helpers ---------------------------------------------------------------

// Parses a decimal number at the start of `text` (leading spaces skipped). Leaves `out` untouched on failure.
bool number_at(const std::string& text, double& out) {
  const char* begin = text.c_str();
  while (*begin == ' ' || *begin == '\t') ++begin;
  char* end = nullptr;
  const double value = std::strtod(begin, &end);
  if (end == begin || !std::isfinite(value)) return false;
  out = value;
  return true;
}

// "30000/1001" or "25" -> 25 / 29.97, if it is a usable frame rate.
std::optional<double> parse_rate(const std::string& text) {
  double numerator = 0, denominator = 1;
  const auto slash = text.find('/');
  if (slash == std::string::npos) {
    if (!number_at(text, numerator)) return std::nullopt;
  } else if (!number_at(text.substr(0, slash), numerator) || !number_at(text.substr(slash + 1), denominator)) {
    return std::nullopt;
  }
  if (!(numerator > 0) || !(denominator > 0)) return std::nullopt;
  return numerator / denominator;
}

// ffmpeg prints floats in C locale; keep the filter expressions in the same format.
std::string num(double value) { return std::to_string(value); }

double round_ms(double seconds) { return std::round(seconds * 1000.0) / 1000.0; }

// The interesting part of ffmpeg's stderr, squeezed into one line for a tool error.
std::string ffmpeg_detail(const std::string& stderr_text) {
  std::vector<std::string> lines;
  std::istringstream in(stderr_text);
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty()) lines.push_back(line);
  }
  if (lines.empty()) return "ffmpeg failed";
  constexpr size_t kMaxLines = 4;
  if (lines.size() > kMaxLines) lines.erase(lines.begin(), lines.end() - static_cast<long>(kMaxLines));
  std::string detail;
  for (auto& l : lines) detail += (detail.empty() ? "" : "; ") + l;
  if (detail.size() > 600) detail = detail.substr(0, 600) + "...";
  return detail;
}

// ---- probe helpers ----------------------------------------------------------------------------

const json& streams_of(const json& info) {
  static const json empty = json::array();
  return info.contains("streams") ? info.at("streams") : empty;
}

const json* first_stream(const json& info, const std::string& type) {
  for (const auto& stream : streams_of(info))
    if (stream.value("type", "") == type) return &stream;
  return nullptr;
}

// Probes an absolute path; probe_media throws a readable ToolError if the file is missing or unreadable.
json probe(const std::string& path) { return probe_media(path); }

// ---- scene changes ----------------------------------------------------------------------------

std::vector<SceneChange> find_scene_changes(const std::string& path, double score) {
  // select keeps only frames whose scene score is above the threshold; metadata=print reports both the
  // timestamp and the score on stderr ("pts_time:1" followed by "lavfi.scene_score=0.4").
  const std::string filter = "select='gt(scene," + num(score) + ")',metadata=print";
  auto r = run({ffmpeg_bin(), "-hide_banner", "-nostats", "-i", path, "-vf", filter, "-an", "-f", "null", "-"});
  if (r.exit_code != 0) throw mcp::ToolError("ffmpeg could not analyse " + path + ": " + ffmpeg_detail(r.err));

  std::vector<SceneChange> hits;
  std::istringstream lines(r.err);
  std::string line;
  double current = -1;
  while (std::getline(lines, line)) {
    const auto time_at = line.find("pts_time:");
    const auto score_at = line.find("lavfi.scene_score=");
    if (time_at != std::string::npos) {
      double time = 0;
      if (number_at(line.substr(time_at + 9), time)) current = time;
    } else if (score_at != std::string::npos) {
      double value = 0;
      if (current >= 0 && number_at(line.substr(score_at + 18), value)) hits.push_back({current, value});
    }
  }
  return hits;
}

// Sorted, in-range, and with near-duplicate changes collapsed (only the first of a cluster is kept).
std::vector<SceneChange> tidy_scenes(std::vector<SceneChange> hits, double duration) {
  std::sort(hits.begin(), hits.end(), [](const SceneChange& a, const SceneChange& b) { return a.time < b.time; });
  std::vector<SceneChange> out;
  for (auto& hit : hits) {
    if (hit.time <= 0) continue;  // the first frame is not a change
    if (duration > 0 && hit.time >= duration) continue;
    if (!out.empty() && hit.time - out.back().time < kMinSceneGap) continue;
    out.push_back(hit);
  }
  return out;
}

std::vector<SceneChange> scene_changes(const std::string& path, double duration, double score) {
  return tidy_scenes(find_scene_changes(path, score), duration);
}

// ---- silence ----------------------------------------------------------------------------------

std::vector<Span> find_silences(const std::string& path, double noise_db, double min_duration, double duration) {
  const std::string filter = "silencedetect=noise=" + num(noise_db) + "dB:d=" + num(min_duration);
  auto r = run({ffmpeg_bin(), "-hide_banner", "-nostats", "-i", path, "-af", filter, "-f", "null", "-"});
  if (r.exit_code != 0) throw mcp::ToolError("ffmpeg could not analyse " + path + ": " + ffmpeg_detail(r.err));

  std::vector<Span> spans;
  std::optional<double> start;
  std::istringstream lines(r.err);
  std::string line;
  while (std::getline(lines, line)) {
    const auto start_at = line.find("silence_start:");
    const auto end_at = line.find("silence_end:");
    if (start_at != std::string::npos) {
      double value = 0;
      if (number_at(line.substr(start_at + 14), value)) start = value;
    } else if (end_at != std::string::npos) {
      double end = 0;
      if (start && number_at(line.substr(end_at + 12), end)) spans.push_back({*start, end});
      start.reset();
    }
  }
  if (start) spans.push_back({*start, duration});  // silence that runs to the end of the file

  for (auto& span : spans) {
    span.start = std::max(0.0, span.start);
    if (duration > 0) span.end = std::min(span.end, duration);
    if (span.end < span.start) std::swap(span.start, span.end);
  }
  spans.erase(std::remove_if(spans.begin(), spans.end(), [](const Span& s) { return s.length() <= kEps; }), spans.end());
  std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.start < b.start; });

  std::vector<Span> merged;  // silencedetect should not overlap, but never report nested spans
  for (auto& span : spans) {
    if (!merged.empty() && span.start <= merged.back().end + kEps) {
      merged.back().end = std::max(merged.back().end, span.end);
    } else {
      merged.push_back(span);
    }
  }
  return merged;
}

// The parts of [0, duration) that are not covered by `gaps`.
std::vector<Span> complement(double duration, const std::vector<Span>& gaps) {
  std::vector<Span> out;
  double cursor = 0;
  for (auto& gap : gaps) {
    const double start = std::max(0.0, gap.start), end = std::min(duration, gap.end);
    if (start > cursor + kEps) out.push_back({cursor, start});
    cursor = std::max(cursor, end);
  }
  if (cursor < duration - kEps) out.push_back({cursor, duration});
  return out;
}

// Grows every span by `padding` (clamped to the source), merges the overlaps that creates, then drops
// anything still shorter than `min_length`.
std::vector<Span> pad_merge_filter(std::vector<Span> spans, double padding, double min_length, double duration) {
  for (auto& span : spans) {
    span.start = std::max(0.0, span.start - padding);
    span.end = std::min(duration, span.end + padding);
  }
  std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.start < b.start; });

  std::vector<Span> merged;
  for (auto& span : spans) {
    if (!merged.empty() && span.start <= merged.back().end + kEps) {
      merged.back().end = std::max(merged.back().end, span.end);
    } else {
      merged.push_back(span);
    }
  }
  merged.erase(std::remove_if(merged.begin(), merged.end(),
                              [min_length](const Span& s) { return s.length() < min_length - kEps; }),
               merged.end());
  return merged;
}

// Keep every span between scene cuts.
std::vector<Span> scene_segments(double duration, const std::vector<SceneChange>& cuts, double min_length) {
  std::vector<double> bounds{0};
  for (auto& cut : cuts)
    if (cut.time > 0 && cut.time < duration) bounds.push_back(cut.time);
  bounds.push_back(duration);

  std::vector<Span> out;
  for (size_t i = 0; i + 1 < bounds.size(); ++i) {
    const Span span{bounds[i], bounds[i + 1]};
    if (span.length() >= min_length - kEps && span.length() > kEps) out.push_back(span);
  }
  return out;
}

// Both inputs sorted and disjoint; the result is too.
std::vector<Span> intersect(const std::vector<Span>& left, const std::vector<Span>& right, double min_length) {
  std::vector<Span> out;
  size_t i = 0, j = 0;
  while (i < left.size() && j < right.size()) {
    const Span span{std::max(left[i].start, right[j].start), std::min(left[i].end, right[j].end)};
    if (span.length() >= min_length - kEps && span.length() > kEps) out.push_back(span);
    if (left[i].end < right[j].end) {
      ++i;
    } else {
      ++j;
    }
  }
  return out;
}

// ---- rough cut --------------------------------------------------------------------------------

void require_open_project(mcp::Server& server) {
  try {
    server.call("get_timeline", json::object());
  } catch (const mcp::ToolError& e) {
    throw mcp::ToolError(std::string(e.what()) +
                         " (or pass create_project=true and project_path to start a new project here)");
  }
}

json auto_rough_cut(mcp::Server& server, const json& a) {
  if (!a.contains("path") || !a["path"].is_string()) throw std::invalid_argument("path is required");
  const std::string path = absolute(a["path"].get<std::string>());
  const std::string mode = a.value("mode", "silence");
  const double noise_db = a.value("noise_db", -30.0);
  const double min_silence = a.value("min_silence", 0.5);
  const double min_segment = a.value("min_segment", 0.4);
  const double padding = a.value("padding", 0.1);
  const bool create_project = a.value("create_project", false);

  if (mode != "silence" && mode != "scene" && mode != "both")
    throw std::invalid_argument("mode must be \"silence\", \"scene\" or \"both\" (got \"" + mode + "\")");
  if (!(min_segment > 0)) throw std::invalid_argument("min_segment must be > 0 (seconds)");
  if (!(min_silence > 0)) throw std::invalid_argument("min_silence must be > 0 (seconds)");
  if (!(padding >= 0)) throw std::invalid_argument("padding must be >= 0 (seconds)");
  if (!std::isfinite(noise_db)) throw std::invalid_argument("noise_db must be a finite number of dB");

  std::string project_path;
  if (create_project) {
    if (!a.contains("project_path") || !a["project_path"].is_string() || a["project_path"].get<std::string>().empty())
      throw std::invalid_argument("project_path is required when create_project is true");
    project_path = absolute(a["project_path"].get<std::string>());
  } else {
    require_open_project(server);  // fail before the (slower) media analysis
  }

  const json info = probe(path);
  const double duration = info.value("duration_sec", 0.0);
  if (!(duration > 0)) throw mcp::ToolError("cannot cut " + path + ": it has no measurable duration");
  if (!first_stream(info, "video"))
    throw mcp::ToolError("auto_rough_cut needs a video source: " + path +
                         " has no video stream (use add_music for audio-only files)");
  if (mode != "scene" && !first_stream(info, "audio"))
    throw mcp::ToolError("auto_rough_cut in mode=" + mode + " needs an audio stream: " + path +
                         " has no audio (use mode=scene to cut at scene changes only)");

  std::vector<Span> segments;
  if (mode == "silence") {
    const auto silences = find_silences(path, noise_db, min_silence, duration);
    segments = pad_merge_filter(complement(duration, silences), padding, min_segment, duration);
  } else {
    const auto cuts = scene_changes(path, duration, kDefaultSceneThreshold / 100.0);
    if (mode == "scene") {
      segments = scene_segments(duration, cuts, min_segment);
    } else {  // both: cut at scene changes, then remove the silences inside each scene
      const auto silences = find_silences(path, noise_db, min_silence, duration);
      const auto speech = pad_merge_filter(complement(duration, silences), padding, min_segment, duration);
      segments = intersect(scene_segments(duration, cuts, 0), speech, min_segment);
    }
  }

  for (auto& segment : segments) {
    segment.start = round_ms(segment.start);
    segment.end = round_ms(segment.end);
  }
  segments.erase(std::remove_if(segments.begin(), segments.end(), [](const Span& s) { return s.length() <= kEps; }),
                 segments.end());
  if (segments.empty()) throw mcp::ToolError("no segments survived the cut; the source may be too short");

  if (create_project) {
    const json* video = first_stream(info, "video");
    const int width = video ? video->value("width", 0) : 0;
    const int height = video ? video->value("height", 0) : 0;
    auto fps = video ? parse_rate(video->value("frame_rate", "")) : std::nullopt;
    json created = {{"project_path", project_path},
                    {"name", std::filesystem::path(path).stem().string()},
                    {"width", width > 0 ? width : 1920},
                    {"height", height > 0 ? height : 1080},
                    {"fps", fps.value_or(30.0)}};
    server.call("create_project", created);
  }
  for (auto& segment : segments) {
    // add_clip imports the source (deduped by path) and appends at the end of the sequence.
    server.call("add_clip", json{{"path", path}, {"in", segment.start}, {"out", segment.end}});
  }

  json listed = json::array();
  double kept = 0;
  for (auto& segment : segments) {
    listed.push_back({{"in", segment.start}, {"out", segment.end}, {"length", round_ms(segment.length())}});
    kept += segment.length();
  }
  return {{"path", path},
          {"mode", mode},
          {"segments", listed},
          {"clips_added", segments.size()},
          {"source_duration", round_ms(duration)},
          {"kept_seconds", round_ms(kept)},
          {"removed_seconds", round_ms(std::max(0.0, duration - kept))}};
}

}  // namespace

void register_analysis_tools(mcp::Server& server) {
  server.add_tool({"detect_scenes",
                   "Find scene changes (hard cuts) in a video with ffmpeg's scene score. threshold is a 0-100 "
                   "percentage (default 8) mapped to the 0-1 score ffmpeg compares against, so threshold=8 means "
                   "a scene score above 0.08. Returns the score actually used, the source duration, and the scene "
                   "changes sorted by time (changes closer than 0.1 s are reported once).",
                   props({{"path", {{"type", "string"}, {"description", "Path to a video file"}}},
                          {"threshold", {{"type", "number"},
                                         {"description", "Sensitivity 0-100 (percent of the 0-1 scene score); "
                                                         "default 8, lower finds more cuts"}}}},
                         {"path"}),
                   [](const json& a) {
                     return guarded([&] {
                       const json info = probe(absolute(a.at("path").get<std::string>()));
                       const std::string path = info.at("path").get<std::string>();
                       const double threshold = a.value("threshold", kDefaultSceneThreshold);
                       if (!std::isfinite(threshold) || threshold < 0 || threshold > 100)
                         throw std::invalid_argument("threshold must be within [0, 100] (it is a percentage)");

                       const double duration = info.value("duration_sec", 0.0);
                       if (!first_stream(info, "video"))
                         throw mcp::ToolError("detect_scenes needs a video file: " + path + " has no video stream");

                       const double score = threshold / 100.0;
                       const auto hits = scene_changes(path, duration, score);
                       json scenes = json::array();
                       for (auto& hit : hits) {
                         json scene = {{"time", round_ms(hit.time)}};
                         if (hit.score) scene["score"] = *hit.score;
                         scenes.push_back(scene);
                       }
                       return json{{"path", path},
                                   {"threshold", score},
                                   {"duration_sec", duration},
                                   {"scenes", scenes},
                                   {"count", scenes.size()}};
                     });
                   }});

  server.add_tool({"detect_silence",
                   "Find silent stretches with ffmpeg's silencedetect. noise_db is the silence floor in dB "
                   "(default -30) and min_duration the shortest silence to report in seconds (default 0.5). "
                   "Returns the silences plus speech, the complement of the silences over the source, so a tool "
                   "or an edit can keep only the talking parts.",
                   props({{"path", {{"type", "string"}, {"description", "Path to a file with an audio stream"}}},
                          {"noise_db", {{"type", "number"},
                                        {"description", "Everything below this level counts as silence, in dB "
                                                        "(default -30)"}}},
                          {"min_duration", {{"type", "number"},
                                            {"description", "Shortest silence to report, in seconds (default 0.5)"}}}},
                         {"path"}),
                   [](const json& a) {
                     return guarded([&] {
                       const json info = probe(absolute(a.at("path").get<std::string>()));
                       const std::string path = info.at("path").get<std::string>();
                       const double noise_db = a.value("noise_db", -30.0);
                       const double min_duration = a.value("min_duration", 0.5);
                       if (!std::isfinite(noise_db)) throw std::invalid_argument("noise_db must be a finite number");
                       if (!(min_duration > 0)) throw std::invalid_argument("min_duration must be > 0 (seconds)");

                       const double duration = info.value("duration_sec", 0.0);
                       if (!(duration > 0)) throw mcp::ToolError("cannot analyse " + path + ": it has no measurable duration");
                       if (!first_stream(info, "audio"))
                         throw mcp::ToolError("no audio stream found in " + path +
                                              ": silence detection needs an audio track");

                       const auto silences = find_silences(path, noise_db, min_duration, duration);
                       const auto speech = complement(duration, silences);

                       json silence_list = json::array();
                       double silence_total = 0;
                       for (auto& span : silences) {
                         silence_list.push_back(
                             {{"start", span.start}, {"end", span.end}, {"duration", span.length()}});
                         silence_total += span.length();
                       }
                       json speech_list = json::array();
                       double speech_total = 0;
                       for (auto& span : speech) {
                         speech_list.push_back({{"start", span.start}, {"end", span.end}});
                         speech_total += span.length();
                       }
                       return json{{"path", path},
                                   {"duration_sec", duration},
                                   {"silences", silence_list},
                                   {"speech", speech_list},
                                   {"silence_total", silence_total},
                                   {"speech_total", speech_total}};
                     });
                   }});

  server.add_tool({"auto_rough_cut",
                   "Rough-cut a long take into a first edit. mode=silence keeps the speech (silences found by "
                   "detect_silence are removed, padding seconds are kept around each kept part), mode=scene cuts at "
                   "scene changes (with detect_scenes' default threshold) and keeps everything between them, "
                   "mode=both does both. Segments shorter than "
                   "min_segment are dropped. With create_project=true a new project is created at project_path "
                   "(size and frame rate taken from the source) and the segments are added as clips; otherwise the "
                   "clips are appended to the project that is already open. Returns the kept segments and the kept "
                   "and removed seconds.",
                   props({{"path", {{"type", "string"}, {"description", "Path to a video file"}}},
                          {"mode", {{"type", "string"},
                                    {"enum", {"silence", "scene", "both"}},
                                    {"description", "silence (default), scene, or both"}}},
                          {"noise_db", {{"type", "number"}, {"description", "Silence floor in dB (default -30)"}}},
                          {"min_silence", {{"type", "number"},
                                           {"description", "Shortest silence to cut out, in seconds (default 0.5)"}}},
                          {"min_segment", {{"type", "number"},
                                           {"description", "Drop kept segments shorter than this, in seconds (default 0.4)"}}},
                          {"padding", {{"type", "number"},
                                       {"description", "Seconds kept on each side of the speech (default 0.1)"}}},
                          {"create_project", {{"type", "boolean"},
                                              {"description", "Create a new project instead of using the open one"}}},
                          {"project_path", {{"type", "string"},
                                            {"description", "Where to save the new project (.json); required when "
                                                            "create_project is true"}}}},
                         {"path"}),
                   [&server](const json& a) { return guarded([&] { return auto_rough_cut(server, a); }); }});
}

}  // namespace et::tools
