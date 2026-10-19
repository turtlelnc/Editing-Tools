// Tests for the whisper.cpp integration. The model is never run: a fake whisper-cli script writes a fixed
// transcript, so these tests cover argument construction, audio extraction, JSON parsing and timeline mapping.
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "mcp/server.h"
#include "process.h"
#include "tools/all.h"
#include "tools/session.h"

using namespace et;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using mcp::json;
namespace fs = std::filesystem;

namespace {

int next_id() {
  static int n = 0;
  return n++;
}

struct Env {
  std::string name, old;
  bool had = false;
  Env(const char* key, const std::string& value) : name(key) {
    const char* p = std::getenv(key);
    had = p != nullptr;
    if (had) old = p;
    setenv(key, value.c_str(), 1);
  }
  ~Env() {
    if (had) setenv(name.c_str(), old.c_str(), 1);
    else unsetenv(name.c_str());
  }
};

// A fake whisper-cli: records its arguments and writes a fixed two-segment JSON transcript.
fs::path write_fake_whisper(const fs::path& dir) {
  const fs::path script = dir / "fake-whisper.sh";
  std::ofstream(script) << R"SH(#!/bin/sh
printf '%s\n' "$*" > "$WHISPER_LOG"
of=""
while [ $# -gt 0 ]; do
  case "$1" in
    -of) of="$2"; shift 2 ;;
    *) shift ;;
  esac
done
cat > "$of.json" <<'JSON'
{"result":{"language":"zh"},"transcription":[
 {"offsets":{"from":1000,"to":2000},"text":" 你好，这是第一句。"},
 {"offsets":{"from":2500,"to":3000},"text":" 这是第二句。"}]}
JSON
)SH";
  std::error_code ec;
  fs::permissions(script, fs::perms::owner_all, fs::perm_options::replace, ec);
  return script;
}

struct Fixture {
  fs::path dir;
  std::string video, silent_video, model, log;

  Fixture() {
    dir = fs::temp_directory_path() / ("et_transcribe_" + std::to_string(::getpid()) + "_" + std::to_string(next_id()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    video = (dir / "speech.mp4").string();
    silent_video = (dir / "mute.mp4").string();
    model = (dir / "ggml-tiny.bin").string();
    log = (dir / "whisper-args.txt").string();
    std::ofstream(model) << "not a real model";
    REQUIRE(run({"ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i", "testsrc=size=160x120:rate=10:duration=4",
                 "-f", "lavfi", "-i", "sine=frequency=440:duration=4", "-shortest", "-pix_fmt", "yuv420p", video})
                .exit_code == 0);
    REQUIRE(run({"ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i", "testsrc=size=160x120:rate=10:duration=2",
                 "-pix_fmt", "yuv420p", silent_video})
                .exit_code == 0);
  }
  ~Fixture() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

struct Harness {
  Fixture& f;
  mcp::Server server{"t", "0"};
  std::shared_ptr<tools::Session> session = std::make_shared<tools::Session>();
  explicit Harness(Fixture& fixture) : f(fixture) {
    tools::register_all_tools(server, session);
    fake_whisper = write_fake_whisper(f.dir);
  }
  fs::path fake_whisper;
  void configure_whisper() {
    env_whisper = std::make_unique<Env>("EDITING_TOOLS_WHISPER", fake_whisper.string());
    env_model = std::make_unique<Env>("EDITING_TOOLS_WHISPER_MODEL", f.model);
    env_log = std::make_unique<Env>("WHISPER_LOG", f.log);
  }
  std::unique_ptr<Env> env_whisper, env_model, env_log;
  json call(const char* name, json args) { return server.call(name, args); }
};

}  // namespace

TEST_CASE("transcribe: runs the configured whisper binary and parses its JSON") {
  Fixture f;
  Harness h(f);
  h.configure_whisper();

  const json result = h.call("transcribe", {{"path", f.video}});
  CHECK(result["language"] == "zh");
  REQUIRE(result["segments"].size() == 2);
  CHECK(result["segments"][0]["start"] == 1.0);
  CHECK(result["segments"][0]["end"] == 2.0);
  CHECK_THAT(result["segments"][0]["text"].get<std::string>(), ContainsSubstring("你好"));
  CHECK_THAT(result["text"].get<std::string>(), ContainsSubstring("第二句"));
  CHECK(result["duration_sec"].get<double>() > 3.5);

  // The binary got the model, the extracted 16 kHz mono wav and JSON output.
  std::ifstream log(f.log);
  const std::string args((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
  CHECK_THAT(args, ContainsSubstring("-m " + f.model));
  CHECK_THAT(args, ContainsSubstring("-l auto"));
  CHECK_THAT(args, ContainsSubstring("-oj"));
  CHECK_THAT(args, ContainsSubstring("audio.wav"));
}

TEST_CASE("transcribe: without configuration the error says how to configure it") {
  Fixture f;
  Harness h(f);
  unsetenv("EDITING_TOOLS_WHISPER_MODEL");
  CHECK_THROWS_WITH(h.call("transcribe", {{"path", f.video}}), ContainsSubstring("EDITING_TOOLS_WHISPER_MODEL"));
}

TEST_CASE("transcribe: a file without audio is rejected clearly") {
  Fixture f;
  Harness h(f);
  h.configure_whisper();
  CHECK_THROWS_WITH(h.call("transcribe", {{"path", f.silent_video}}), ContainsSubstring("audio"));
}

TEST_CASE("auto_subtitles: maps source times through clip in/out and speed onto the timeline") {
  Fixture f;
  Harness h(f);
  h.configure_whisper();
  h.call("create_project", {{"project_path", (f.dir / "p.json").string()}, {"width", 160}, {"height", 120}, {"fps", 10}});
  h.call("add_clip", {{"path", f.video}, {"in", 1}, {"out", 3}});
  h.call("set_speed", {{"clip_id", "c1"}, {"speed", 2}});

  const json project = h.call("auto_subtitles", json::object());
  REQUIRE(project["subtitles"].size() == 2);
  // Source 1.0-2.0 s inside the clip becomes 0.0-0.5 s on the timeline at 2x; 2.5-3.0 s becomes 0.75-1.0 s.
  CHECK_THAT(project["subtitles"][0]["start"].get<double>(), WithinAbs(0.0, 0.01));
  CHECK_THAT(project["subtitles"][0]["end"].get<double>(), WithinAbs(0.5, 0.01));
  CHECK_THAT(project["subtitles"][1]["start"].get<double>(), WithinAbs(0.75, 0.01));
  CHECK_THAT(project["subtitles"][1]["end"].get<double>(), WithinAbs(1.0, 0.01));
  CHECK_THAT(project["subtitles"][0]["text"].get<std::string>(), ContainsSubstring("你好"));
  CHECK(project["duration"].get<double>() == 1.0);

  // A second call reuses the cached transcript instead of running whisper again.
  h.call("auto_subtitles", json::object());
  std::ifstream log(f.log);
  const std::string args((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
  CHECK(h.session->transcripts.size() == 1);
}

TEST_CASE("auto_subtitles: needs a project and a clip that actually has speech") {
  Fixture f;
  Harness h(f);
  h.configure_whisper();
  CHECK_THROWS_WITH(h.call("auto_subtitles", json::object()), ContainsSubstring("no project open"));

  h.call("create_project", {{"project_path", (f.dir / "p.json").string()}, {"width", 160}, {"height", 120}, {"fps", 10}});
  h.call("add_clip", {{"path", f.silent_video}});
  CHECK_THROWS_WITH(h.call("auto_subtitles", json::object()), ContainsSubstring("no speech"));
}
