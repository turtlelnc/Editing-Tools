#include "tools/transcribe.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "process.h"
#include "tools/media.h"
#include "util/activity.h"

namespace et::tools {

namespace fs = std::filesystem;

namespace {

std::string env_or(const char* name) {
  const char* value = std::getenv(name);
  return (value && *value) ? value : "";
}

std::string whisper_bin() {
  const std::string configured = env_or("EDITING_TOOLS_WHISPER");
  return configured.empty() ? "whisper-cli" : configured;
}

void require_whisper_configured() {
  const std::string model = env_or("EDITING_TOOLS_WHISPER_MODEL");
  if (model.empty())
    throw mcp::ToolError(
        "speech recognition is not configured: set EDITING_TOOLS_WHISPER_MODEL to a whisper.cpp ggml model "
        "(e.g. .../models/ggml-base.bin) and EDITING_TOOLS_WHISPER to the whisper-cli binary if it is not on PATH");
  std::error_code ec;
  if (!fs::is_regular_file(model, ec))
    throw mcp::ToolError("the whisper model file does not exist: " + model);
  const std::string bin = whisper_bin();
  if (bin.find('/') != std::string::npos && !fs::is_regular_file(bin, ec))
    throw mcp::ToolError("the whisper binary does not exist: " + bin + " (set EDITING_TOOLS_WHISPER)");
}

// A throw-away directory, removed however the call ends.
struct TempDir {
  fs::path path;
  TempDir() {
    static std::atomic<int> counter{0};
    path = fs::temp_directory_path() /
           ("et_whisper_" + std::to_string(static_cast<long long>(::getpid())) + "_" + std::to_string(counter++));
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) throw mcp::ToolError("cannot create a temporary folder for transcription: " + ec.message());
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

std::string trimmed(const std::string& text) {
  size_t begin = 0, end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return text.substr(begin, end - begin);
}

struct Segment {
  double start = 0, end = 0;  // seconds in the source file
  std::string text;
};

std::string read_text(const fs::path& file) {
  std::ifstream f(file, std::ios::binary);
  std::stringstream buffer;
  buffer << f.rdbuf();
  return buffer.str();
}

// Transcribes one media file with the configured whisper.cpp binary. Returns segments in SOURCE time.
json transcribe_file(const std::string& media, const std::string& language) {
  require_whisper_configured();
  const std::string model = env_or("EDITING_TOOLS_WHISPER_MODEL");
  const std::string bin = whisper_bin();

  TempDir tmp;
  const fs::path wav = tmp.path / "audio.wav";
  auto extract = run({ffmpeg_bin(), "-y", "-v", "error", "-i", media, "-vn", "-ac", "1", "-ar", "16000", "-c:a",
                      "pcm_s16le", wav.string()});
  if (extract.exit_code != 0) {
    std::string why = trimmed(extract.err);
    if (why.size() > 300) why = why.substr(0, 300) + "…";
    throw mcp::ToolError("could not extract audio from " + media + " (does it have an audio stream?): " + why);
  }

  const std::string lang = language.empty() ? "auto" : language;
  activity::report("transcribing audio with whisper…");
  const fs::path prefix = tmp.path / "result";
  auto whisper = run({bin, "-m", model, "-f", wav.string(), "-l", lang, "-oj", "-of", prefix.string()});
  if (whisper.exit_code == 127)
    throw mcp::ToolError("whisper-cli was not found: install whisper.cpp or point EDITING_TOOLS_WHISPER at it");

  const fs::path result_file = fs::path(prefix.string() + ".json");
  const std::string raw = read_text(result_file);
  if (raw.empty()) {
    std::string why = trimmed(whisper.err);
    if (why.size() > 400) why = why.substr(0, 400) + "…";
    throw mcp::ToolError("whisper did not produce a transcript" +
                         std::string(whisper.exit_code != 0 ? " (exit " + std::to_string(whisper.exit_code) + ")" : "") +
                         (why.empty() ? "" : ": " + why));
  }

  json parsed = json::parse(raw, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object())
    throw mcp::ToolError("whisper wrote an unreadable transcript: " + result_file.string());

  std::string detected = "unknown";
  if (parsed.contains("result") && parsed["result"].is_object() && parsed["result"].contains("language") &&
      parsed["result"]["language"].is_string())
    detected = parsed["result"]["language"].get<std::string>();

  json segments = json::array();
  std::string text;
  for (const auto& item : parsed.value("transcription", json::array())) {
    if (!item.is_object()) continue;
    double start = 0, end = 0;
    if (item.contains("offsets") && item["offsets"].is_object()) {
      const json& offsets = item["offsets"];
      if (offsets.contains("from") && offsets["from"].is_number()) start = offsets["from"].get<double>() / 1000.0;
      if (offsets.contains("to") && offsets["to"].is_number()) end = offsets["to"].get<double>() / 1000.0;
    }
    std::string piece = item.contains("text") && item["text"].is_string() ? item["text"].get<std::string>() : "";
    piece = trimmed(piece);
    if (piece.empty() || end <= start) continue;
    segments.push_back({{"start", start}, {"end", end}, {"text", piece}});
    if (!text.empty()) text += " ";
    text += piece;
  }

  return {{"language", detected}, {"segments", segments}, {"text", text}};
}

// Cached transcription for one media file (source times). The cache lives in the tool session.
json transcription(Session& session, const std::string& media, const std::string& language) {
  const std::string key = media + "|" + (language.empty() ? "auto" : language);
  auto it = session.transcripts.find(key);
  if (it != session.transcripts.end()) {
    json cached = json::parse(it->second, nullptr, false);
    if (cached.is_object()) return cached;
  }
  json result = transcribe_file(media, language);
  session.transcripts[key] = result.dump();
  return result;
}

std::vector<model::SubtitleCue> cues_from_transcript(const json& transcript, const model::Clip& clip,
                                                     double timeline_start) {
  std::vector<model::SubtitleCue> cues;
  const double speed = clip.speed > 0 ? clip.speed : 1.0;
  for (const auto& segment : transcript.value("segments", json::array())) {
    if (!segment.is_object()) continue;
    const double seg_start = segment.value("start", 0.0);
    const double seg_end = segment.value("end", 0.0);
    const double start = std::max(seg_start, clip.in);
    const double end = std::min(seg_end, clip.out);
    if (end - start < 0.15) continue;  // too short to read, or outside the clip
    std::string text = trimmed(segment.value("text", std::string()));
    if (text.empty()) continue;
    model::SubtitleCue cue;
    cue.start = timeline_start + (start - clip.in) / speed;
    cue.end = timeline_start + (end - clip.in) / speed;
    cue.text = text;
    if (cue.start < timeline_start) cue.start = timeline_start;
    cues.push_back(std::move(cue));
  }
  return cues;
}

}  // namespace

void register_transcribe_tools(mcp::Server& server, std::shared_ptr<Session> s) {
  server.add_tool({"transcribe",
                   "Speech-to-text for one media file. Returns timestamped segments in SOURCE-file time (not the "
                   "output timeline) plus the full text and detected language. Needs whisper.cpp: set "
                   "EDITING_TOOLS_WHISPER to whisper-cli (if not on PATH) and EDITING_TOOLS_WHISPER_MODEL to a ggml "
                   "model. Use the segments to plan cuts, or auto_subtitles to put the text on the timeline.",
                   props({{"path", kStr}, {"language", kStr}}, {"path"}),
                   [s](const json& a) {
                     return guarded([&] {
                       const std::string path = absolute(a.at("path").get<std::string>());
                       std::error_code ec;
                       if (!fs::is_regular_file(path, ec))
                         throw mcp::ToolError("media file not found: " + path);
                       const auto info = probe_media(path);
                       bool has_audio = false;
                       for (const auto& stream : info.value("streams", json::array()))
                         if (stream.value("type", std::string()) == "audio") has_audio = true;
                       if (!has_audio) throw mcp::ToolError("transcribe needs a file with an audio stream: " + path);

                       const json transcript = transcription(*s, path, a.value("language", std::string()));
                       return json{{"path", path},
                                   {"duration_sec", info.value("duration_sec", 0.0)},
                                   {"language", transcript["language"]},
                                   {"segments", transcript["segments"]},
                                   {"text", transcript["text"]}};
                     });
                   }});

  server.add_tool({"auto_subtitles",
                   "Transcribe the project's clips with whisper and replace ALL subtitles with the recognized text. "
                   "Timeline times, clip in/out and speed are taken into account, so the cues land on the OUTPUT "
                   "timeline. This replaces existing subtitles; use set_subtitles/update_subtitle afterwards to fix "
                   "wording. Needs EDITING_TOOLS_WHISPER_MODEL (and EDITING_TOOLS_WHISPER if whisper-cli is not on "
                   "PATH).",
                   props({{"language", kStr}, {"clip_id", kStr}}),
                   [s](const json& a) {
                     return guarded([&] {
                       auto& project = s->get();
                       const std::string language = a.value("language", std::string());
                       const std::string clip_id = a.value("clip_id", std::string());
                       if (!clip_id.empty() && std::none_of(project.clips.begin(), project.clips.end(),
                                                            [&](const model::Clip& c) { return c.id == clip_id; }))
                         throw mcp::ToolError("unknown clip: " + clip_id);
                       require_whisper_configured();

                       std::vector<model::SubtitleCue> cues;
                       size_t transcribed_assets = 0;
                       std::map<std::string, json> by_asset;  // asset id -> transcript
                       double cursor = 0;
                       for (const auto& clip : project.clips) {
                         const model::Asset& asset = project.asset(clip.asset_id);
                         const double clip_start = cursor;
                         cursor += clip.length();
                         if (!clip_id.empty() && clip.id != clip_id) continue;
                         if (!asset.has_audio) continue;

                         auto found = by_asset.find(asset.id);
                         if (found == by_asset.end()) {
                           activity::report("transcribing " + fs::path(asset.path).filename().string() + "…");
                           found = by_asset.emplace(asset.id, transcription(*s, asset.path, language)).first;
                           ++transcribed_assets;
                         }
                         auto clip_cues = cues_from_transcript(found->second, clip, clip_start);
                         cues.insert(cues.end(), clip_cues.begin(), clip_cues.end());
                       }

                       if (cues.empty())
                         throw mcp::ToolError(
                             "no speech was recognized in the selected clips (check that they have audible speech, "
                             "or try another language)");

                       project.set_subtitles(std::move(cues));
                       s->save();
                       activity::report("added " + std::to_string(project.subtitles.size()) + " subtitle cues");
                       (void)transcribed_assets;
                       return project.to_json();
                     });
                   }});
}

}  // namespace et::tools
