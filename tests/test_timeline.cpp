#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>
#include <filesystem>

#include "compiler/ffmpeg_graph.h"
#include "process.h"
#include "render/render.h"
#include "tools/media.h"

using namespace et;
using Catch::Matchers::ContainsSubstring;

static model::Project sample() {
  model::Project p;
  p.width = 320; p.height = 240; p.fps = 25;
  p.add_asset({"", "/m/a.mp4", 10, true, true, 640, 480});
  p.add_asset({"", "/m/silent.mp4", 5, true, false, 640, 480});
  return p;
}

TEST_CASE("model validates edits and keeps state on failure") {
  auto p = sample();
  p.add_clip("a1", 1, 4);
  p.add_clip("a2", 0, 2);
  REQUIRE(p.duration() == 5);
  REQUIRE_THROWS(p.add_clip("a1", 0, 11));
  REQUIRE_THROWS(p.add_clip("a1", 3, 3));
  REQUIRE_THROWS(p.add_clip("nope", 0, 1));
  REQUIRE_THROWS(p.trim_clip("c1", 0, 99));
  REQUIRE(p.clips[0].out == 4);
  p.move_clip("c2", 0);
  REQUIRE(p.clips[0].id == "c2");
  p.delete_clip("c2");
  REQUIRE(p.clips.size() == 1);
  REQUIRE(p.add_asset({"", "/m/a.mp4", 10, true, true, 640, 480}).id == "a1");  // deduped
}

TEST_CASE("model round-trips through JSON") {
  auto p = sample();
  p.add_clip("a1", 1, 4);
  auto q = model::Project::from_json(p.to_json());
  REQUIRE(q.to_json() == p.to_json());
}

TEST_CASE("compiler emits trim/concat graph and synthesizes silence for silent sources") {
  auto p = sample();
  p.add_clip("a1", 1, 4);
  p.add_clip("a2", 0, 2);
  auto args = compiler::compile(p, {"/out.mp4", false});
  std::string joined;
  for (auto& a : args) joined += a + "\n";
  REQUIRE_THAT(joined, ContainsSubstring("[0:v]trim=start=1.000000:end=4.000000"));
  REQUIRE_THAT(joined, ContainsSubstring("anullsrc"));
  REQUIRE_THAT(joined, ContainsSubstring("concat=n=2:v=1:a=1[v][dlg]"));
  REQUIRE_THROWS(compiler::compile(model::Project{}, {"/o.mp4", false}));
}

TEST_CASE("end to end: render two cuts and verify duration with ffprobe") {
  namespace fs = std::filesystem;
  auto dir = fs::temp_directory_path() / "et_e2e";
  fs::create_directories(dir);
  auto src = (dir / "src.mp4").string(), silent = (dir / "silent.mp4").string(), out = (dir / "out.mp4").string();
  REQUIRE(run({"ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i", "testsrc=size=640x360:rate=30:duration=3", "-f", "lavfi",
               "-i", "sine=duration=3", "-shortest", "-pix_fmt", "yuv420p", src}).exit_code == 0);
  REQUIRE(run({"ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i", "testsrc=size=160x120:rate=10:duration=2", "-pix_fmt",
               "yuv420p", silent}).exit_code == 0);

  model::Project p;
  p.width = 320; p.height = 240; p.fps = 25;
  auto a = p.add_asset({"", src, 3, true, true, 640, 360}).id;
  auto b = p.add_asset({"", silent, 2, true, false, 160, 120}).id;
  p.add_clip(a, 0.5, 2.0);
  p.add_clip(b, 0, 1.0);
  render::render(p, {out, true});

  auto info = tools::probe_media(out);
  REQUIRE(info["duration_sec"].get<double>() == Catch::Approx(2.5).margin(0.15));
  REQUIRE(info["streams"].size() == 2);
  REQUIRE(info["streams"][0]["width"] == 320);
  REQUIRE(info["streams"][0]["height"] == 240);
  fs::remove_all(dir);
}

// ---- audio ----

static std::string joined_args(const model::Project& p) {
  std::string j;
  for (auto& a : compiler::compile(p, {"/out.mp4", false})) j += a + "\n";
  return j;
}

TEST_CASE("audio model validation") {
  auto p = sample();
  p.add_asset({"", "/m/song.mp3", 30, false, true, 0, 0});
  p.add_clip("a1", 0, 4);
  REQUIRE_THROWS(p.add_music("a3", 0, 40, 0));      // beyond source
  REQUIRE_THROWS(p.add_music("a2", 0, 3, 0));       // silent video has no audio
  REQUIRE_THROWS(p.add_music("a3", 0, 10, -1));     // negative start
  p.add_music("a3", 0, 10, 2, -12);
  REQUIRE_THROWS(p.set_audio("m1", {std::nullopt, 6.0, 5.0, std::nullopt}));  // fades exceed length
  REQUIRE_THROWS(p.set_audio("c1", {99.0, std::nullopt, std::nullopt, std::nullopt}));
  REQUIRE_THROWS(p.set_audio("zz", {}));
  p.set_audio("m1", {std::nullopt, 2.0, 3.0, true});
  REQUIRE(p.music[0].audio.volume_db == -12);  // untouched fields kept
  REQUIRE(p.music[0].audio.fade_out == 3.0);
}

TEST_CASE("audio model round-trips and old projects still load") {
  auto p = sample();
  p.add_asset({"", "/m/song.mp3", 30, false, true, 0, 0});
  p.add_clip("a1", 0, 4);
  p.add_music("a3", 1, 9, 0.5, -6);
  p.set_audio("c1", {-3.0, 0.5, 0.5, true});
  p.set_duck(true, "strong");
  p.set_loudnorm(true, -14, -1);
  REQUIRE(model::Project::from_json(p.to_json()).to_json() == p.to_json());

  auto j = p.to_json();
  j.erase("music"); j.erase("duck"); j.erase("loudnorm");
  j["clips"][0].erase("audio");
  auto old = model::Project::from_json(j);
  REQUIRE(old.music.empty());
  REQUIRE(old.clips[0].audio.volume_db == 0);
}

TEST_CASE("compiler wires music, ducking, fades and loudnorm") {
  auto p = sample();
  p.add_asset({"", "/m/song.mp3", 30, false, true, 0, 0});
  p.add_clip("a1", 0, 8);
  p.add_music("a3", 0, 10, 1.5, -10);
  p.set_audio("m1", {std::nullopt, 1.0, 2.0, std::nullopt});

  auto plain = joined_args(p);
  REQUIRE_THAT(plain, ContainsSubstring("volume=-10.000000dB,afade=t=in:st=0:d=1.000000,afade=t=out:st=8.000000:d=2.000000"));
  REQUIRE_THAT(plain, ContainsSubstring("adelay=delays=1500:all=1"));
  REQUIRE_THAT(plain, ContainsSubstring("[dlg][mus]amix=inputs=2:duration=first:normalize=0[mix]"));
  REQUIRE_THAT(plain, !ContainsSubstring("sidechaincompress"));
  REQUIRE_THAT(plain, !ContainsSubstring("loudnorm"));

  p.set_duck(true, "strong");
  p.set_loudnorm(true, -16, -1.5);
  auto full = joined_args(p);
  REQUIRE_THAT(full, ContainsSubstring("sidechaincompress=threshold=0.02:ratio=12"));
  REQUIRE_THAT(full, ContainsSubstring("loudnorm=I=-16.000000:TP=-1.500000"));
  REQUIRE_THAT(full, ContainsSubstring("-i\n/m/song.mp3"));
}

namespace {
namespace fs = std::filesystem;

// Mean level (dB) of the 1 kHz band within [ss, ss+t) of a file's audio.
double band_db(const std::string& file, double ss, double t) {
  auto r = run({"ffmpeg", "-v", "info", "-nostdin", "-ss", std::to_string(ss), "-t", std::to_string(t), "-i", file, "-af",
                "bandpass=f=1000:width_type=h:w=200,volumedetect", "-f", "null", "-"});
  auto pos = r.err.find("mean_volume:");
  REQUIRE(pos != std::string::npos);
  return std::stod(r.err.substr(pos + 12));
}

double integrated_lufs(const std::string& file) {
  auto r = run({"ffmpeg", "-v", "info", "-nostdin", "-i", file, "-af", "ebur128", "-f", "null", "-"});
  auto pos = r.err.rfind("I:");
  REQUIRE(pos != std::string::npos);
  return std::stod(r.err.substr(pos + 2));
}

struct AudioFixture {
  fs::path dir = fs::temp_directory_path() / "et_audio";
  std::string video = (dir / "dialog.mp4").string(), song = (dir / "song.wav").string();
  AudioFixture() {
    fs::create_directories(dir);
    // 4 s video: 300 Hz "speech" for the first 2 s, then silence.
    REQUIRE(run({"ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i", "testsrc=size=160x120:rate=10:duration=4", "-f", "lavfi",
                 "-i", "sine=frequency=300:duration=2", "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo:d=2",
                 "-filter_complex",
                 "[1:a]aresample=48000,aformat=channel_layouts=stereo[s];[2:a]aformat=channel_layouts=stereo[z];"
                 "[s][z]concat=n=2:v=0:a=1[a]",
                 "-map", "0:v", "-map", "[a]", "-pix_fmt", "yuv420p", "-t", "4", video}).exit_code == 0);
    REQUIRE(run({"ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i", "sine=frequency=1000:duration=4", song}).exit_code == 0);
  }
  ~AudioFixture() { fs::remove_all(dir); }

  model::Project project() {
    model::Project p;
    p.width = 160; p.height = 120; p.fps = 10;
    auto v = p.add_asset({"", video, 4, true, true, 160, 120}).id;
    auto s = p.add_asset({"", song, 4, false, true, 0, 0}).id;
    p.add_clip(v, 0, 4);
    p.add_music(s, 0, 4, 0);
    return p;
  }
  std::string render(const model::Project& p, const std::string& name) {
    auto out = (dir / name).string();
    render::render(p, {out, true});
    return out;
  }
};
}  // namespace

TEST_CASE("music volume_db changes the rendered level by that amount") {
  AudioFixture f;
  auto p = f.project();
  auto loud = band_db(f.render(p, "loud.mp4"), 2.5, 1.0);
  p.set_audio("m1", {-20.0, std::nullopt, std::nullopt, std::nullopt});
  auto quiet = band_db(f.render(p, "quiet.mp4"), 2.5, 1.0);
  REQUIRE(loud - quiet == Catch::Approx(20.0).margin(1.5));
}

TEST_CASE("auto_duck lowers music under speech and recovers afterwards") {
  AudioFixture f;
  auto p = f.project();
  auto base = f.render(p, "base.mp4");
  p.set_duck(true, "strong");
  auto ducked = f.render(p, "ducked.mp4");

  double under_speech = band_db(base, 0.5, 1.0) - band_db(ducked, 0.5, 1.0);
  double after_speech = band_db(base, 3.0, 1.0) - band_db(ducked, 3.0, 1.0);
  REQUIRE(under_speech > 6.0);
  REQUIRE(std::abs(after_speech) < 1.5);
}

TEST_CASE("normalize_loudness brings the mix near the target") {
  AudioFixture f;
  auto p = f.project();
  p.set_audio("m1", {-25.0, std::nullopt, std::nullopt, std::nullopt});
  p.set_loudnorm(true, -16, -1.5);
  REQUIRE(integrated_lufs(f.render(p, "norm.mp4")) == Catch::Approx(-16.0).margin(1.5));
}
