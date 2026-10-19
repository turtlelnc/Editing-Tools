#include <unistd.h>

#include <atomic>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>

#include "compiler/ffmpeg_graph.h"
#include "mcp/server.h"
#include "process.h"
#include "render/render.h"
#include "tools/color.h"
#include "tools/timeline.h"
#include "util/activity.h"
#include "util/base64.h"

using namespace et;
using Catch::Matchers::ContainsSubstring;
using mcp::json;

static model::Project sample() {
  model::Project p;
  p.width = 320; p.height = 240; p.fps = 25;
  p.add_asset({"", "/m/a.mp4", 10, true, true, 640, 480});
  p.add_clip("a1", 0, 4);
  p.add_clip("a1", 4, 8);
  return p;
}

static std::string joined(const std::vector<std::string>& v) {
  std::string j;
  for (auto& a : v) j += a + "\n";
  return j;
}

TEST_CASE("color_adjust validates ranges and applies atomically") {
  auto p = sample();
  p.set_color("", {{"saturation", 0.5}, {"warmth", 0.3}});
  p.set_color("c1", {{"brightness", 0.1}});
  REQUIRE(p.grade.saturation == 0.5);
  REQUIRE(p.clips[0].grade.brightness == 0.1);
  REQUIRE(p.clips[1].grade.neutral());

  REQUIRE_THROWS(p.set_color("", {{"saturation", 0.2}, {"brightness", 2}}));
  REQUIRE(p.grade.saturation == 0.5);  // nothing from the failed call stuck
  REQUIRE_THROWS(p.set_color("", {{"contrast", -1}}));
  REQUIRE_THROWS(p.set_color("", {{"gamma", 0}}));
  REQUIRE_THROWS(p.set_color("", {{"hue", 1}}));
  REQUIRE_THROWS(p.set_color("nope", {{"tint", 0.1}}));

  p.reset_color("");
  REQUIRE(p.grade.neutral());
  REQUIRE(p.clips[0].grade.brightness == 0.1);  // clip grade untouched by project reset
}

TEST_CASE("curves and LUT validation") {
  auto p = sample();
  REQUIRE_THROWS(p.set_curve("", "nonsense", {}));
  REQUIRE_THROWS(p.set_curve("", "darker", {{0, 0}, {1, 1}}));        // both given
  REQUIRE_THROWS(p.set_curve("", "", {{0, 0}}));                       // too few points
  REQUIRE_THROWS(p.set_curve("", "", {{0, 0}, {0.5, 0.5}, {0.5, 0.6}}));  // x not increasing
  REQUIRE_THROWS(p.set_curve("", "", {{0, 0}, {1, 1.5}}));
  p.set_curve("", "", {{0, 0}, {0.5, 0.7}, {1, 1}});
  REQUIRE(p.grade.curve_points.size() == 3);
  p.set_curve("", "vintage", {});
  REQUIRE(p.grade.curve_points.empty());  // preset replaces points
  p.set_curve("", "", {});
  REQUIRE(p.grade.curve_preset.empty());

  REQUIRE_THROWS(p.set_lut("", "/x/look.png", 1));
  REQUIRE_THROWS(p.set_lut("", "/x/look.cube", 1.5));
  p.set_lut("c1", "/x/Look.CUBE", 0.5);
  REQUIRE(p.clips[0].grade.lut_intensity == 0.5);
}

TEST_CASE("color survives a JSON round trip; old projects still load") {
  auto p = sample();
  p.set_color("", {{"warmth", 0.4}});
  p.set_curve("c2", "", {{0, 0}, {0.5, 0.6}, {1, 1}});
  p.set_lut("c1", "/x/look.cube", 0.75);
  REQUIRE(model::Project::from_json(p.to_json()).to_json() == p.to_json());

  auto j = p.to_json();
  j.erase("color");
  for (auto& c : j["clips"]) c.erase("color");
  auto old = model::Project::from_json(j);
  REQUIRE(old.grade.neutral());
  REQUIRE(old.clips[0].grade.neutral());
}

TEST_CASE("compiler: grade chain order, project grade before clip grade") {
  auto p = sample();
  p.set_color("", {{"warmth", 0.5}, {"tint", -0.5}, {"saturation", 1.5}});
  p.set_curve("", "", {{0, 0}, {0.5, 0.7}, {1, 1}});
  p.set_color("c2", {{"brightness", 0.1}});
  auto j = joined(compiler::compile(p, {"/o.mp4", false, ""}));
  REQUIRE_THAT(j, ContainsSubstring("[pre0]colortemperature=temperature=5000.000000,colorchannelmixer=gg=1.100000,"
                                    "eq=brightness=0.000000:contrast=1.000000:saturation=1.500000:gamma=1.000000,"
                                    "curves=master='0.000000/0.000000 0.500000/0.700000 1.000000/1.000000'[gp0p]"));
  REQUIRE_THAT(j, ContainsSubstring("[gp1p]eq=brightness=0.100000"));  // c2: project grade first, then its own
  REQUIRE_THAT(j, ContainsSubstring("[gc1p]scale=320:240"));
  REQUIRE_THAT(j, ContainsSubstring("[gp0p]scale=320:240"));           // c1 has no clip grade
}

TEST_CASE("compiler: warmth maps to colour temperature, wider on the cool side") {
  auto p = sample();
  p.set_color("", {{"warmth", -0.5}});
  REQUIRE_THAT(joined(compiler::compile(p, {"/o.mp4", false, ""})), ContainsSubstring("colortemperature=temperature=13250.000000"));
  p.set_color("", {{"warmth", 1.0}});
  REQUIRE_THAT(joined(compiler::compile(p, {"/o.mp4", false, ""})), ContainsSubstring("colortemperature=temperature=3500.000000"));
}

TEST_CASE("compiler: LUT full strength vs blended, and unsafe LUT paths are refused") {
  auto p = sample();
  p.set_lut("c1", "/Volumes/My Drive/看起来 好.cube", 1.0);
  REQUIRE_THROWS(compiler::compile(p, {"/o.mp4", false, ""}));  // would need escaping: must be aliased first

  compiler::Options o;
  o.output = "/o.mp4";
  o.lut_paths["/Volumes/My Drive/看起来 好.cube"] = "/tmp/et_1_0.cube";
  auto full = joined(compiler::compile(p, o));
  REQUIRE_THAT(full, ContainsSubstring("lut3d=file='/tmp/et_1_0.cube'"));
  REQUIRE_THAT(full, !ContainsSubstring("blend"));

  p.set_lut("c1", "/Volumes/My Drive/看起来 好.cube", 0.4);
  auto mix = joined(compiler::compile(p, o));
  REQUIRE_THAT(mix, ContainsSubstring("split[gc0a][gc0b]"));
  REQUIRE_THAT(mix, ContainsSubstring("blend=all_expr='A*(1-0.400000)+B*0.400000'"));

  p.set_lut("c1", "/Volumes/My Drive/看起来 好.cube", 0.0);
  REQUIRE_THAT(joined(compiler::compile(p, o)), !ContainsSubstring("lut3d"));  // intensity 0 = no-op
}

TEST_CASE("compiler: single-frame mode reads one clip, seeks the input, and drops audio") {
  auto p = sample();
  compiler::Options o;
  o.output = "/f.jpg";
  o.frame_time = 6.0;  // second clip (starts at 4), 2 s in
  o.frame_max_width = 480;
  auto j = joined(compiler::compile(p, o));
  REQUIRE_THAT(j, ContainsSubstring("-i\n/m/a.mp4"));
  REQUIRE_THAT(j, ContainsSubstring("-ss\n4.000000\n-i\n/m/a.mp4"));  // jump straight to the clip's source start
  REQUIRE_THAT(j, ContainsSubstring("trim=start=0.000000:end=4.000000"));  // times are relative to the seek point
  REQUIRE_THAT(j, ContainsSubstring("-map\n[vfit]\n-ss\n2.000000"));      // frame is 2 s into the clip
  REQUIRE_THAT(j, ContainsSubstring("concat=n=1:v=1:a=0[v]"));
  REQUIRE_THAT(j, !ContainsSubstring("atrim"));
  REQUIRE_THAT(j, ContainsSubstring("scale='min(480,iw)':-2"));
  REQUIRE_THAT(j, ContainsSubstring("mjpeg"));

  o.frame_time = 7.9;  // 3.9 s into clip 2 -> seek 0.9 s past clip start
  j = joined(compiler::compile(p, o));
  REQUIRE_THAT(j, ContainsSubstring("-ss\n4.900000\n-i\n/m/a.mp4"));
  REQUIRE_THROWS(compiler::compile(p, [&] { auto x = o; x.frame_time = 8.0; return x; }()));  // end is exclusive
}

TEST_CASE("compiler: subtitles in frame mode are shifted to timeline time and back") {
  auto p = sample();
  p.set_subtitles({{"", 5, 6, "x"}});
  compiler::Options o;
  o.output = "/f.jpg";
  o.ass_path = "/tmp/x.ass";
  o.frame_time = 7.9;
  auto j = joined(compiler::compile(p, o));
  REQUIRE_THAT(j, ContainsSubstring("setpts=PTS+4.900000/TB,ass=filename='/tmp/x.ass',setpts=PTS-4.900000/TB"));
}

// ---- real renders ----

namespace {
namespace fs = std::filesystem;

struct Rgb { int r, g, b; };

// Average colour of the frame at `t` seconds of any video or image file.
Rgb avg_rgb(const std::string& file, double t = 0) {
  std::vector<std::string> cmd = {"ffmpeg", "-v", "error", "-nostdin"};
  // Single images must not be seeked: ffmpeg's image demuxer can return nothing for -ss 0.
  if (fs::path(file).extension() != ".jpg") cmd.insert(cmd.end(), {"-ss", std::to_string(t)});
  cmd.insert(cmd.end(), {"-i", file, "-vf", "scale=1:1:flags=area,format=rgb24", "-frames:v", "1", "-f", "rawvideo", "-"});
  auto r = run(cmd);
  REQUIRE(r.exit_code == 0);
  REQUIRE(r.out.size() == 3);
  return {static_cast<unsigned char>(r.out[0]), static_cast<unsigned char>(r.out[1]), static_cast<unsigned char>(r.out[2])};
}

struct ColorFixture {
  fs::path dir = fs::temp_directory_path() / ("et_color_" + std::to_string(::getpid()));
  std::string orange = (dir / "orange.mp4").string(), gray = (dir / "gray.mp4").string(), redblue = (dir / "redblue.mp4").string();

  ColorFixture() {
    fs::create_directories(dir);
    auto make = [&](const std::string& src, const std::string& out) {
      REQUIRE(run({"ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i", src, "-f", "lavfi", "-i", "sine=duration=2", "-shortest",
                   "-pix_fmt", "yuv420p", "-g", "5", out}).exit_code == 0);
    };
    make("color=c=0xC04020:s=160x120:r=10:d=2", orange);
    make("color=c=0x808080:s=160x120:r=10:d=2", gray);
    // 8 s: red for 4 s, then blue for 4 s, short GOP so seeks are cheap.
    REQUIRE(run({"ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i", "color=c=red:s=160x120:r=10:d=4", "-f", "lavfi", "-i",
                 "color=c=blue:s=160x120:r=10:d=4", "-filter_complex", "[0][1]concat=n=2:v=1:a=0[v]", "-map", "[v]", "-pix_fmt",
                 "yuv420p", "-g", "5", redblue}).exit_code == 0);
  }
  ~ColorFixture() { fs::remove_all(dir); }

  model::Project project(const std::string& file, double dur) {
    model::Project p;
    p.width = 160; p.height = 120; p.fps = 10;
    p.add_clip(p.add_asset({"", file, dur, true, file != redblue, 160, 120}).id, 0, dur);
    return p;
  }
  Rgb render_avg(const model::Project& p, const std::string& name, double t = 0.5) {
    auto out = (dir / ("out_" + name)).string();
    render::render(p, {out, true, ""});
    return avg_rgb(out, t);
  }
};
}  // namespace

TEST_CASE("rendered color: saturation, brightness, warmth") {
  ColorFixture f;
  auto p = f.project(f.orange, 2);
  auto base = f.render_avg(p, "base.mp4");
  REQUIRE(base.r - base.b > 120);  // sanity: the source really is orange

  p.set_color("", {{"saturation", 0}});
  auto bw = f.render_avg(p, "bw.mp4");
  CHECK(std::abs(bw.r - bw.g) < 8);
  CHECK(std::abs(bw.g - bw.b) < 8);

  p.reset_color("");
  p.set_color("", {{"brightness", 0.3}});
  auto bright = f.render_avg(p, "bright.mp4");
  CHECK(bright.r + bright.g + bright.b > base.r + base.g + base.b + 60);

  auto g = f.project(f.gray, 2);
  auto gray = f.render_avg(g, "gray.mp4");
  g.set_color("", {{"warmth", 1}});
  auto warm = f.render_avg(g, "warm.mp4");
  g.set_color("", {{"warmth", -1}});
  auto cool = f.render_avg(g, "cool.mp4");
  CHECK(warm.r - warm.b > gray.r - gray.b + 30);
  CHECK(cool.b - cool.r > gray.b - gray.r + 30);
}

TEST_CASE("rendered color: custom curve lifts midtones") {
  ColorFixture f;
  auto g = f.project(f.gray, 2);
  auto base = f.render_avg(g, "base.mp4");
  g.set_curve("", "", {{0, 0}, {0.5, 0.8}, {1, 1}});
  auto lifted = f.render_avg(g, "lift.mp4");
  CHECK(lifted.g > base.g + 40);
  g.set_curve("", "darker", {});
  auto darker = f.render_avg(g, "dark.mp4");
  CHECK(darker.g < base.g - 10);
}

TEST_CASE("rendered color: LUT from a path with spaces and CJK, at full and half strength") {
  ColorFixture f;
  auto lutdir = f.dir / "my luts 外置硬盘";
  fs::create_directories(lutdir);
  auto lut = (lutdir / "swap rb.cube").string();
  {  // swaps red and blue: output (r,g,b) = (b,g,r); red varies fastest
    std::ofstream o(lut);
    o << "TITLE \"swap\"\nLUT_3D_SIZE 2\n";
    for (int b = 0; b < 2; ++b)
      for (int g = 0; g < 2; ++g)
        for (int r = 0; r < 2; ++r) o << b << ".0 " << g << ".0 " << r << ".0\n";
  }
  auto p = f.project(f.orange, 2);
  auto base = f.render_avg(p, "base.mp4");

  p.set_lut("", lut, 1.0);
  auto full = f.render_avg(p, "full.mp4");
  CHECK(std::abs(full.r - base.b) < 14);
  CHECK(std::abs(full.b - base.r) < 14);

  p.set_lut("", lut, 0.5);
  auto half = f.render_avg(p, "half.mp4");
  CHECK(std::abs(half.r - (base.r + base.b) / 2) < 14);
  CHECK(std::abs(half.b - (base.r + base.b) / 2) < 14);

  fs::remove(lut);
  REQUIRE_THROWS_WITH(f.render_avg(p, "gone.mp4"), ContainsSubstring("LUT file not found"));
  REQUIRE_THROWS_WITH(f.render_avg(p, "gone.mp4"), ContainsSubstring("drive"));
}

TEST_CASE("rendered color: project-wide grade hits every clip, clip grade only its own") {
  ColorFixture f;
  model::Project p;
  p.width = 160; p.height = 120; p.fps = 10;
  auto a = p.add_asset({"", f.orange, 2, true, true, 160, 120}).id;
  p.add_clip(a, 0, 1);
  p.add_clip(a, 1, 2);
  p.set_color("c2", {{"saturation", 0}});
  auto out = (f.dir / "two.mp4").string();
  render::render(p, {out, true, ""});
  auto c1 = avg_rgb(out, 0.5), c2 = avg_rgb(out, 1.5);
  CHECK(c1.r - c1.b > 100);
  CHECK(std::abs(c2.r - c2.b) < 10);

  p.set_color("", {{"saturation", 0}});
  render::render(p, {out, true, ""});
  CHECK(std::abs(avg_rgb(out, 0.5).r - avg_rgb(out, 0.5).b) < 10);
}

TEST_CASE("frame preview: right clip, right moment, JPEG, scaled") {
  ColorFixture f;
  model::Project p;
  p.width = 160; p.height = 120; p.fps = 10;
  p.add_clip(p.add_asset({"", f.redblue, 8, true, false, 160, 120}).id, 1, 8);  // source 1..8, 7 s on the timeline

  auto look = [&](double t, int max_width) {
    auto jpeg = render::frame(p, t, max_width);
    REQUIRE(jpeg.size() > 100);
    REQUIRE(static_cast<unsigned char>(jpeg[0]) == 0xFF);
    REQUIRE(static_cast<unsigned char>(jpeg[1]) == 0xD8);
    auto path = (f.dir / "frame.jpg").string();
    std::ofstream(path, std::ios::binary) << jpeg;
    return path;
  };
  auto red = avg_rgb(look(2.5, 0));   // source 3.5 s: still red
  CHECK(red.r > 200); CHECK(red.b < 60);
  auto edge = avg_rgb(look(3.2, 0));  // source 4.2 s: just turned blue
  CHECK(edge.b > 200); CHECK(edge.r < 60);
  auto late = avg_rgb(look(5.0, 0));  // source 6.0 s: needs the input seek
  CHECK(late.b > 200);

  auto small = look(0.0, 64);
  auto probe = run({"ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height", "-of", "csv=p=0", small});
  CHECK(probe.out.substr(0, 3) == "64,");  // scaled down to the requested width

  REQUIRE_THROWS(render::frame(p, 7.0, 0));   // timeline is 7 s long: end is exclusive
  REQUIRE_THROWS(render::frame(p, -1.0, 0));
}

TEST_CASE("frame preview applies grading and shows subtitles only at their time") {
  ColorFixture f;
  model::Project p;
  p.width = 160; p.height = 120; p.fps = 10;
  p.add_clip(p.add_asset({"", f.gray, 2, true, true, 160, 120}).id, 0, 2);
  p.set_color("", {{"saturation", 0}, {"brightness", 0.2}});
  auto path = (f.dir / "f.jpg").string();
  std::ofstream(path, std::ios::binary) << render::frame(p, 1.0, 0);
  CHECK(avg_rgb(path).g > 128 + 25);  // graded frame is brighter than the 0x80 source

  if (!ffmpeg_has_filter("ass")) SKIP("ffmpeg lacks libass; set EDITING_TOOLS_FFMPEG to one that has it");
  p.reset_color("");
  p.set_subtitles({{"", 1.5, 2.0, "WWWWWWWWWWWWWWWW"}});
  p.set_subtitle_style({{"font_size", 30}, {"box", true}, {"outline_color", "#FFFFFF"}, {"color", "#000000"}, {"box_opacity", 1.0}});
  auto with = [&](double t) {
    std::ofstream(path, std::ios::binary) << render::frame(p, t, 0);
    return avg_rgb(path);
  };
  CHECK(with(1.0).g < 140);        // cue not on screen yet: still gray 0x80
  CHECK(with(1.7).g > 150);        // cue on screen: white box raises the average
}

TEST_CASE("MCP: color tools and preview_frame return an image") {
  ColorFixture f;
  mcp::Server server("t", "0");
  auto session = std::make_shared<tools::Session>();
  tools::register_timeline_tools(server, session);
  tools::register_color_tools(server, session);
  int id = 0;
  auto call = [&](const char* name, json args) {
    return server.handle({{"jsonrpc", "2.0"}, {"id", ++id}, {"method", "tools/call"},
                          {"params", {{"name", name}, {"arguments", args}}}})["result"];
  };
  REQUIRE(call("create_project", {{"project_path", (f.dir / "p.json").string()}, {"width", 160}, {"height", 120}})["isError"] == false);
  REQUIRE(call("add_clip", {{"path", f.orange}})["isError"] == false);
  REQUIRE(call("color_adjust", {{"saturation", 0}})["isError"] == false);
  REQUIRE(call("color_curves", {{"points", {{0, 0}, {0.5, 0.6}, {1, 1}}}})["isError"] == false);

  auto r = call("preview_frame", {{"time", 0.5}, {"max_width", 80}});
  REQUIRE(r["isError"] == false);
  REQUIRE(r["content"][0]["type"] == "image");
  REQUIRE(r["content"][0]["mimeType"] == "image/jpeg");
  REQUIRE(r["content"][0]["data"].get<std::string>().rfind("/9j/", 0) == 0);  // base64 of a JPEG header
  REQUIRE(r["content"][1]["type"] == "text");

  REQUIRE(call("preview_frame", {{"time", 99}})["isError"] == true);
  REQUIRE(call("color_adjust", {{"saturation", 9}})["isError"] == true);
  REQUIRE(call("apply_lut", {{"path", "/Volumes/not mounted/look.cube"}})["content"][0]["text"].get<std::string>().find("drive") != std::string::npos);
}

TEST_CASE("MCP: preview_frames returns one contact sheet; preview_compare shows before/after") {
  ColorFixture f;
  mcp::Server server("t", "0");
  auto session = std::make_shared<tools::Session>();
  tools::register_timeline_tools(server, session);
  tools::register_color_tools(server, session);
  int id = 0;
  auto call = [&](const char* name, json args) {
    return server.handle({{"jsonrpc", "2.0"},
                          {"id", ++id},
                          {"method", "tools/call"},
                          {"params", {{"name", name}, {"arguments", args}}}})["result"];
  };
  REQUIRE(call("create_project", {{"project_path", (f.dir / "p.json").string()}, {"width", 160}, {"height", 120}, {"fps", 10}})["isError"] == false);
  REQUIRE(call("add_clip", {{"path", f.orange}})["isError"] == false);
  REQUIRE(call("color_adjust", {{"saturation", 0}})["isError"] == false);

  const auto sheet = call("preview_frames", {{"times", {0.2, 0.8, 1.4}}, {"max_width", 120}, {"columns", 3}});
  REQUIRE(sheet["isError"] == false);
  REQUIRE(sheet["content"][0]["type"] == "image");
  const std::string sheet_path = (f.dir / "sheet.jpg").string();
  std::ofstream(sheet_path, std::ios::binary) << util::unbase64(sheet["content"][0]["data"].get<std::string>());
  const auto size = run({"ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height",
                         "-of", "csv=p=0", sheet_path});
  CHECK(size.out.find("360,90") == 0);  // three 120px cells side by side

  // A single frame is still one image, and bad times are rejected.
  REQUIRE(call("preview_frames", {{"times", {0.5}}})["isError"] == false);
  REQUIRE(call("preview_frames", {{"times", {99}}})["isError"] == true);
  REQUIRE(call("preview_frames", {{"times", {}}})["isError"] == true);

  const auto cmp = call("preview_compare", {{"time", 0.5}, {"max_width", 80}});
  REQUIRE(cmp["isError"] == false);
  REQUIRE(cmp["content"][0]["type"] == "image");
  const std::string cmp_path = (f.dir / "cmp.jpg").string();
  std::ofstream(cmp_path, std::ios::binary) << util::unbase64(cmp["content"][0]["data"].get<std::string>());
  const auto cmp_size = run({"ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height",
                             "-of", "csv=p=0", cmp_path});
  CHECK(cmp_size.out.find("160,60") == 0);  // two 80px halves
  CHECK_THAT(cmp["content"][1]["text"].get<std::string>(), ContainsSubstring("left"));
}

TEST_CASE("render reports progress and can be cancelled mid-flight") {
  ColorFixture f;
  auto p = f.project(f.orange, 2);
  const auto out = (f.dir / "progress.mp4").string();
  {
    activity::Recorder recorder;
    render::render(p, {out, true, ""});
    REQUIRE_FALSE(recorder.lines.empty());
    CHECK_THAT(recorder.lines.back(), ContainsSubstring("100%"));
  }
  CHECK(fs::exists(out));

  // A long enough render that cancelling on the first progress update really kills ffmpeg.
  const std::string long_file = (f.dir / "long.mp4").string();
  REQUIRE(run({"ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i", "color=c=blue:s=640x480:r=25:d=40", "-c:v", "libx264",
               "-preset", "ultrafast", "-pix_fmt", "yuv420p", long_file}).exit_code == 0);
  model::Project big;
  big.width = 640;
  big.height = 480;
  big.fps = 25;
  big.add_clip(big.add_asset({"", long_file, 40, true, false, 640, 480}).id, 0, 40);
  const auto cancelled_out = (f.dir / "cancelled.mp4").string();
  {
    std::atomic<bool> cancel{false};
    activity::Scope scope([&](const std::string&) { cancel = true; }, [&] { return cancel.load(); });
    REQUIRE_THROWS_WITH(render::render(big, {cancelled_out, false, ""}), ContainsSubstring("cancelled"));
  }
  std::error_code ec;
  const bool finished = fs::exists(cancelled_out, ec) && fs::file_size(cancelled_out, ec) > 100000;
  CHECK_FALSE(finished);  // the partial file was never a finished render
}
TEST_CASE("MCP: apply_style applies presets and set_speed changes the rendered length") {
  ColorFixture f;
  mcp::Server server("t", "0");
  auto session = std::make_shared<tools::Session>();
  tools::register_timeline_tools(server, session);
  tools::register_color_tools(server, session);
  int id = 0;
  auto call = [&](const char* name, json args) {
    return server.handle({{"jsonrpc", "2.0"},
                          {"id", ++id},
                          {"method", "tools/call"},
                          {"params", {{"name", name}, {"arguments", args}}}})["result"];
  };
  REQUIRE(call("create_project", {{"project_path", (f.dir / "p.json").string()}, {"width", 160}, {"height", 120}, {"fps", 10}})["isError"] == false);
  REQUIRE(call("add_clip", {{"path", f.orange}})["isError"] == false);

  REQUIRE(call("apply_style", {{"preset", "warm_cinematic"}})["isError"] == false);
  const auto warm = session->get();
  CHECK(warm.grade.warmth > 0.3);
  CHECK(warm.grade.contrast > 1.0);
  REQUIRE(call("apply_style", {{"preset", "black_and_white"}, {"clip_id", "c1"}})["isError"] == false);
  CHECK(session->get().clips[0].grade.saturation == 0.0);
  REQUIRE(call("apply_style", {{"preset", "nonsense"}})["isError"] == true);

  // 2x speed halves the rendered file; 0.5x doubles it.
  REQUIRE(call("set_speed", {{"clip_id", "c1"}, {"speed", 2.0}})["isError"] == false);
  const auto fast_out = (f.dir / "fast.mp4").string();
  REQUIRE(call("render_preview", {{"output_path", fast_out}})["isError"] == false);
  const auto fast = run({"ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", fast_out});
  CHECK(std::stod(fast.out) > 0.7);
  CHECK(std::stod(fast.out) < 1.3);

  REQUIRE(call("set_speed", {{"clip_id", "c1"}, {"speed", 0.5}})["isError"] == false);
  const auto slow_out = (f.dir / "slow.mp4").string();
  REQUIRE(call("render_preview", {{"output_path", slow_out}})["isError"] == false);
  const auto slow = run({"ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", slow_out});
  CHECK(std::stod(slow.out) > 3.2);
  CHECK(std::stod(slow.out) < 4.8);
  REQUIRE(call("set_speed", {{"clip_id", "c1"}, {"speed", 9}})["isError"] == true);
}
