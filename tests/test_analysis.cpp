// Tests for the analysis tools (detect_scenes, detect_silence, auto_rough_cut).
//
// The fixtures are tiny lavfi-generated files: a two-colour clip with one hard cut, audio with silence
// in the middle / at the start / at the end, and a "talking head" video whose middle second is silent.
#include <unistd.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "mcp/server.h"
#include "process.h"
#include "tools/all.h"

using Catch::Matchers::ContainsSubstring;
using namespace et;

namespace fs = std::filesystem;
using mcp::json;

namespace {

// One server + session per test, exactly like a fresh process serving tool calls.
struct Harness {
  mcp::Server server{"test", "0"};
  std::shared_ptr<tools::Session> session = std::make_shared<tools::Session>();
  Harness() { tools::register_all_tools(server, session); }
  json call(const std::string& name, const json& args) { return server.call(name, args); }
};

void generate(const std::vector<std::string>& argv, const std::string& path) {
  auto r = run(argv);
  if (r.exit_code != 0) throw std::runtime_error("ffmpeg could not create " + path + ": " + r.err);
}

// Built once for the whole binary; all paths live under one temp directory that is removed at exit.
struct Fixtures {
  fs::path dir = fs::temp_directory_path() / ("et_analysis_" + std::to_string(::getpid()));
  std::string scenes = (dir / "scenes.mp4").string();   // 1 s red + 1 s blue: one cut at 1.0 s
  std::string silence = (dir / "silence.m4a").string();  // tone, 1 s silence, tone
  std::string lead = (dir / "lead.m4a").string();        // silence starts at 0
  std::string trail = (dir / "trail.m4a").string();      // silence runs to the end
  std::string talk = (dir / "talk.mp4").string();        // 3 s of video with the same 1 s silence
  std::string tiny = (dir / "tiny.mp4").string();        // 0.3 s, too short to survive a 0.4 s minimum

  Fixtures() {
    fs::remove_all(dir);
    fs::create_directories(dir);
    generate({"ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i", "color=red:size=160x120:rate=10:duration=1",
              "-f", "lavfi", "-i", "color=blue:size=160x120:rate=10:duration=1", "-filter_complex",
              "[0][1]concat=n=2:v=1:a=0", "-pix_fmt", "yuv420p", scenes},
             scenes);
    generate({"ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i", "sine=440:duration=3", "-af",
              "volume=enable='between(t,1,2)':volume=0", "-c:a", "aac", silence},
             silence);
    generate({"ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i", "sine=440:duration=2", "-af",
              "volume=enable='lt(t,1)':volume=0", "-c:a", "aac", lead},
             lead);
    generate({"ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i", "sine=440:duration=2", "-af",
              "volume=enable='gte(t,1)':volume=0", "-c:a", "aac", trail},
             trail);
    generate({"ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i", "color=gray:size=160x120:rate=10:duration=3",
              "-f", "lavfi", "-i", "sine=440:duration=3", "-filter_complex",
              "[1:a]volume=enable='between(t,1,2)':volume=0[a]", "-map", "0:v", "-map", "[a]", "-c:v", "libx264",
              "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest", talk},
             talk);
    generate({"ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i", "color=black:size=64x64:rate=10:duration=0.3",
              "-pix_fmt", "yuv420p", tiny},
             tiny);
  }
  ~Fixtures() { fs::remove_all(dir); }
};

const Fixtures& fixtures() {
  static const Fixtures f;
  return f;
}

}  // namespace

TEST_CASE("analysis: detect_scenes finds the cut between two concatenated colours") {
  Harness h;
  const auto& f = fixtures();

  auto r = h.call("detect_scenes", {{"path", f.scenes}});
  CHECK(r["threshold"].get<double>() == Catch::Approx(0.08));  // 8 percent -> 0-1 score
  CHECK(r["duration_sec"].get<double>() == Catch::Approx(2.0).margin(0.15));
  CHECK(r["count"].get<int>() == 1);
  REQUIRE(r["scenes"].size() == 1);
  CHECK(r["scenes"][0]["time"].get<double>() == Catch::Approx(1.0).margin(0.15));
  CHECK(r["scenes"][0].contains("score"));
  CHECK(r["scenes"][0]["score"].get<double>() > 0.08);

  // A high threshold maps to a high score, so the same cut is no longer reported.
  auto high = h.call("detect_scenes", {{"path", f.scenes}, {"threshold", 90}});
  CHECK(high["threshold"].get<double>() == Catch::Approx(0.9));
  CHECK(high["count"].get<int>() == 0);
  CHECK(high["scenes"].empty());
}

TEST_CASE("analysis: detect_silence reports silences and the speech complement") {
  Harness h;
  const auto& f = fixtures();

  auto r = h.call("detect_silence", {{"path", f.silence}});
  const double duration = r["duration_sec"].get<double>();
  CHECK(duration == Catch::Approx(3.0).margin(0.15));
  REQUIRE(r["silences"].size() == 1);
  CHECK(r["silences"][0]["start"].get<double>() == Catch::Approx(1.0).margin(0.15));
  CHECK(r["silences"][0]["end"].get<double>() == Catch::Approx(2.0).margin(0.15));
  CHECK(r["silences"][0]["duration"].get<double>() == Catch::Approx(1.0).margin(0.15));

  REQUIRE(r["speech"].size() == 2);  // before and after the silence
  CHECK(r["speech"][0]["start"].get<double>() == 0);
  CHECK(r["speech"][0]["end"].get<double>() == Catch::Approx(1.0).margin(0.15));
  CHECK(r["speech"][1]["start"].get<double>() == Catch::Approx(2.0).margin(0.15));
  CHECK(r["speech"][1]["end"].get<double>() == Catch::Approx(duration).margin(0.02));
  CHECK(r["silence_total"].get<double>() + r["speech_total"].get<double>() == Catch::Approx(duration).margin(0.05));

  // A minimum duration longer than the gap hides the silence and keeps the whole take.
  auto none = h.call("detect_silence", {{"path", f.silence}, {"min_duration", 1.5}});
  CHECK(none["silences"].empty());
  REQUIRE(none["speech"].size() == 1);
  CHECK(none["speech"][0]["start"].get<double>() == 0);
  CHECK(none["speech"][0]["end"].get<double>() == Catch::Approx(duration).margin(0.02));

  // Silence that starts at 0: no bogus speech span before it.
  auto lead = h.call("detect_silence", {{"path", f.lead}});
  const double lead_duration = lead["duration_sec"].get<double>();
  REQUIRE(lead["silences"].size() == 1);
  CHECK(lead["silences"][0]["start"].get<double>() == Catch::Approx(0.0).margin(0.02));
  REQUIRE(lead["speech"].size() == 1);
  CHECK(lead["speech"][0]["start"].get<double>() == Catch::Approx(1.0).margin(0.15));
  CHECK(lead["speech"][0]["end"].get<double>() == Catch::Approx(lead_duration).margin(0.02));

  // Silence that runs to the end: no bogus speech span after it.
  auto trail = h.call("detect_silence", {{"path", f.trail}});
  const double trail_duration = trail["duration_sec"].get<double>();
  REQUIRE(trail["silences"].size() == 1);
  CHECK(trail["silences"][0]["end"].get<double>() == Catch::Approx(trail_duration).margin(0.02));
  REQUIRE(trail["speech"].size() == 1);
  CHECK(trail["speech"][0]["start"].get<double>() == 0);
  CHECK(trail["speech"][0]["end"].get<double>() == Catch::Approx(1.0).margin(0.15));
}

TEST_CASE("analysis: auto_rough_cut creates a project and appends the kept segments") {
  Harness h;
  const auto& f = fixtures();
  const fs::path project = f.dir / "rough" / "cut.json";
  fs::create_directories(project.parent_path());

  auto r = h.call("auto_rough_cut", {{"path", f.talk}, {"create_project", true}, {"project_path", project.string()}});
  const double source = r["source_duration"].get<double>();
  CHECK(source == Catch::Approx(3.0).margin(0.15));
  CHECK(fs::exists(project));

  // The middle second is silent, so it is cut out (with 0.1 s padding kept on each side).
  REQUIRE(r["segments"].size() >= 2);
  CHECK(r["clips_added"].get<int>() == static_cast<int>(r["segments"].size()));
  CHECK(r["removed_seconds"].get<double>() > 0.5);
  CHECK(r["kept_seconds"].get<double>() == Catch::Approx(source - r["removed_seconds"].get<double>()).margin(0.01));

  double previous_end = 0;
  for (const auto& segment : r["segments"]) {
    const double in = segment["in"].get<double>(), out = segment["out"].get<double>();
    CHECK(in >= 0);
    CHECK(out > in);
    CHECK(out <= source + 1e-6);  // clips never read past the end of the source
    CHECK(segment["length"].get<double>() == Catch::Approx(out - in).margin(0.002));
    CHECK(in >= previous_end - 1e-9);  // sorted, non-overlapping
    previous_end = out;
  }

  // The saved project has one clip per segment and takes its size/fps from the source.
  std::ifstream in(project);
  REQUIRE(in);
  const json saved = json::parse(in);
  REQUIRE(saved["clips"].size() == r["segments"].size());
  CHECK(saved["width"] == 160);
  CHECK(saved["height"] == 120);
  CHECK(saved["fps"].get<double>() == Catch::Approx(10.0).margin(0.01));
  for (size_t i = 0; i < r["segments"].size(); ++i) {
    CHECK(saved["clips"][i]["in"].get<double>() == r["segments"][i]["in"].get<double>());
    CHECK(saved["clips"][i]["out"].get<double>() == r["segments"][i]["out"].get<double>());
  }
  REQUIRE(h.session->project != nullptr);  // the tool is wired to the session it was registered with
  CHECK(h.session->path == tools::absolute(project.string()));
}

TEST_CASE("analysis: auto_rough_cut appends to the project that is already open") {
  Harness h;
  const auto& f = fixtures();
  const fs::path project = f.dir / "append" / "p.json";
  fs::create_directories(project.parent_path());
  h.call("create_project", {{"project_path", project.string()}, {"width", 160}, {"height", 120}, {"fps", 10}});

  auto r = h.call("auto_rough_cut", {{"path", f.talk}});  // create_project defaults to false
  CHECK(r["clips_added"].get<int>() >= 2);

  auto timeline = h.call("get_timeline", json::object());
  CHECK(timeline["clips"].size() == r["segments"].size());
  CHECK(timeline["assets"].size() == 1);  // the source is imported once, not once per clip
  CHECK(timeline["duration"].get<double>() == Catch::Approx(r["kept_seconds"].get<double>()).margin(0.01));
}

TEST_CASE("analysis: auto_rough_cut scene and both modes") {
  Harness h;
  const auto& f = fixtures();
  const fs::path dir = f.dir / "modes";
  fs::create_directories(dir);

  // The talking head is one continuous shot: scene mode keeps the whole take.
  auto scene = h.call("auto_rough_cut", {{"path", f.talk},
                                         {"mode", "scene"},
                                         {"create_project", true},
                                         {"project_path", (dir / "scene.json").string()}});
  REQUIRE(scene["segments"].size() == 1);
  CHECK(scene["segments"][0]["in"].get<double>() == Catch::Approx(0.0).margin(0.01));
  CHECK(scene["segments"][0]["out"].get<double>() == Catch::Approx(scene["source_duration"].get<double>()).margin(0.01));
  CHECK(scene["removed_seconds"].get<double>() == Catch::Approx(0.0).margin(0.01));

  // Both: scene boundaries and silences are respected in one pass.
  auto both = h.call("auto_rough_cut", {{"path", f.talk},
                                        {"mode", "both"},
                                        {"create_project", true},
                                        {"project_path", (dir / "both.json").string()}});
  CHECK(both["clips_added"].get<int>() >= 2);
  CHECK(both["removed_seconds"].get<double>() > 0.5);
}

TEST_CASE("analysis: auto_rough_cut validates its arguments and never leaves a half edit") {
  Harness h;
  const auto& f = fixtures();

  // Nothing is open yet: the tool says so instead of failing halfway through the analysis.
  CHECK_THROWS_WITH(h.call("auto_rough_cut", {{"path", f.talk}}), ContainsSubstring("no project open"));
  CHECK_THROWS_WITH(h.call("auto_rough_cut", {{"path", f.talk}, {"mode", "beat"}}), ContainsSubstring("mode"));
  CHECK_THROWS_WITH(h.call("auto_rough_cut", {{"path", f.talk}, {"min_segment", 0}}), ContainsSubstring("min_segment"));
  CHECK_THROWS_WITH(h.call("auto_rough_cut", {{"path", f.talk}, {"min_silence", -1}}), ContainsSubstring("min_silence"));
  CHECK_THROWS_WITH(h.call("auto_rough_cut", {{"path", f.talk}, {"padding", -0.1}}), ContainsSubstring("padding"));
  CHECK_THROWS_WITH(h.call("auto_rough_cut", {{"path", f.talk}, {"create_project", true}}),
                    ContainsSubstring("project_path"));

  // A source shorter than min_segment has nothing left to keep, and no project is written.
  const fs::path project = f.dir / "never.json";
  CHECK_THROWS_WITH(h.call("auto_rough_cut", {{"path", f.tiny},
                                              {"create_project", true},
                                              {"project_path", project.string()},
                                              {"min_segment", 0.5}}),
                    ContainsSubstring("no segments survived"));
  CHECK_FALSE(fs::exists(project));
}

TEST_CASE("analysis: detect tools reject bad input") {
  Harness h;
  const auto& f = fixtures();

  CHECK_THROWS_AS(h.call("detect_scenes", {{"path", "/no/such/file.mp4"}}), mcp::ToolError);
  CHECK_THROWS_WITH(h.call("detect_scenes", {{"path", f.scenes}, {"threshold", 150}}), ContainsSubstring("threshold"));
  CHECK_THROWS_WITH(h.call("detect_scenes", {{"path", f.silence}}), ContainsSubstring("video"));

  CHECK_THROWS_WITH(h.call("detect_silence", {{"path", f.scenes}}), ContainsSubstring("audio"));
  CHECK_THROWS_WITH(h.call("detect_silence", {{"path", f.silence}, {"min_duration", 0}}),
                    ContainsSubstring("min_duration"));
}
