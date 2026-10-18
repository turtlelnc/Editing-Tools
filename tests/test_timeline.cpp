#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/catch_approx.hpp>
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
  REQUIRE_THAT(joined, ContainsSubstring("concat=n=2:v=1:a=1[v][a]"));
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
