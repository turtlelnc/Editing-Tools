#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>

#include "compiler/ffmpeg_graph.h"
#include "process.h"
#include "render/render.h"
#include "subtitles/ass.h"

using namespace et;
using Catch::Matchers::ContainsSubstring;

static model::Project base_project() {
  model::Project p;
  p.width = 1280; p.height = 720; p.fps = 25;
  p.add_asset({"", "/m/a.mp4", 10, true, true, 1280, 720});
  p.add_clip("a1", 0, 6);
  return p;
}

TEST_CASE("set_subtitles validates, sorts and assigns ids") {
  auto p = base_project();
  REQUIRE_THROWS(p.set_subtitles({{"", 2, 1, "backwards"}}));
  REQUIRE_THROWS(p.set_subtitles({{"", -1, 1, "negative"}}));
  REQUIRE_THROWS(p.set_subtitles({{"", 0, 1, "  \n"}}));
  p.set_subtitles({{"", 3, 4, "second"}, {"", 0, 2, "first"}});
  REQUIRE(p.subtitles[0].text == "first");
  REQUIRE(p.subtitles[0].id == "s1");
  REQUIRE(p.subtitles[1].id == "s2");

  REQUIRE_THROWS(p.update_subtitle("s9", 1.0, std::nullopt, std::nullopt));
  REQUIRE_THROWS(p.update_subtitle("s1", 5.0, std::nullopt, std::nullopt));  // start would pass end
  REQUIRE(p.subtitles[0].start == 0);                                          // unchanged after failure
  p.update_subtitle("s1", 4.5, 5.5, std::string("moved"));
  REQUIRE(p.subtitles[1].text == "moved");  // re-sorted
  p.delete_subtitle("s2");
  REQUIRE(p.subtitles.size() == 1);
  REQUIRE_THROWS(p.delete_subtitle("s2"));
}

TEST_CASE("style patch merges, validates and is atomic") {
  auto p = base_project();
  p.set_subtitle_style({{"color", "#FFCC00"}, {"position", "top"}});
  p.set_subtitle_style({{"bold", true}});
  REQUIRE(p.subtitle_style.color == "#FFCC00");  // earlier change kept
  REQUIRE(p.subtitle_style.bold);
  REQUIRE_THROWS(p.set_subtitle_style({{"bold", false}, {"color", "red"}}));
  REQUIRE(p.subtitle_style.bold);  // failed patch changed nothing
  REQUIRE_THROWS(p.set_subtitle_style({{"position", "left"}}));
  REQUIRE_THROWS(p.set_subtitle_style({{"font_size", 3}}));
  REQUIRE_THROWS(p.set_subtitle_style({{"box_opacity", 2}}));
  REQUIRE_THROWS(p.set_subtitle_style({{"nonsense", 1}}));
}

TEST_CASE("subtitles survive a JSON round trip; old projects still load") {
  auto p = base_project();
  p.set_subtitles({{"", 0, 2, "hello"}});
  p.set_subtitle_style({{"font", "Menlo"}, {"box", true}});
  REQUIRE(model::Project::from_json(p.to_json()).to_json() == p.to_json());

  auto j = p.to_json();
  j.erase("subtitles"); j.erase("subtitle_style");
  auto old = model::Project::from_json(j);
  REQUIRE(old.subtitles.empty());
  REQUIRE(old.subtitle_style.position == "bottom");
}

TEST_CASE("ASS output: colours, alignment, timing, escaping") {
  auto p = base_project();
  p.set_subtitles({{"", 1.234, 3661.5, "line one\nline {two}"}});
  p.set_subtitle_style({{"color", "#FF8800"}, {"outline_color", "#112233"}, {"position", "top"}, {"font", "Menlo"},
                        {"font_size", 40}, {"bold", true}});
  auto ass = subtitles::to_ass(p);
  REQUIRE_THAT(ass, ContainsSubstring("PlayResX: 1280"));
  REQUIRE_THAT(ass, ContainsSubstring("PlayResY: 720"));
  // ASS colours are &HAABBGGRR: #FF8800 -> 0088FF, #112233 -> 332211.
  REQUIRE_THAT(ass, ContainsSubstring("Style: Default,Menlo,40,&H000088FF,&H000088FF,&H00332211,&H80000000,-1,0,0,0,100,100,0,0,1,2.5,1,8,"));
  REQUIRE_THAT(ass, ContainsSubstring("Dialogue: 0,0:00:01.23,1:01:01.50,Default,,0,0,0,,line one\\Nline \\{two\\}"));
}

TEST_CASE("ASS box mode uses an opaque-box border style with the requested opacity") {
  auto p = base_project();
  p.set_subtitles({{"", 0, 1, "x"}});
  p.set_subtitle_style({{"box", true}, {"box_opacity", 0.5}, {"font_size", 50}});
  auto ass = subtitles::to_ass(p);
  REQUIRE_THAT(ass, ContainsSubstring("&H80000000,0,0,0,0,100,100,0,0,3,10,0,2,"));  // BorderStyle 3, padding 10, no shadow
}

TEST_CASE("SRT parse and format") {
  auto cues = subtitles::parse_srt(
      "\xEF\xBB\xBF"
      "1\r\n00:00:01,500 --> 00:00:03,000\r\nHello\r\nworld\r\n\r\n"
      "2\r\n00:01:00.25 --> 00:01:02,5\r\n你好\r\n");
  REQUIRE(cues.size() == 2);
  REQUIRE(cues[0].start == 1.5);
  REQUIRE(cues[0].text == "Hello\nworld");
  REQUIRE(cues[1].start == 60.25);
  REQUIRE(cues[1].end == 62.5);
  REQUIRE(cues[1].text == "你好");

  auto again = subtitles::parse_srt(subtitles::to_srt(cues));
  REQUIRE(again.size() == 2);
  REQUIRE(again[0].text == cues[0].text);
  REQUIRE(again[1].end == cues[1].end);
  REQUIRE_THROWS(subtitles::parse_srt("not a subtitle file"));
}

TEST_CASE("compiler burns subtitles in with the ass filter and maps the result") {
  auto p = base_project();
  p.set_subtitles({{"", 0, 1, "hi"}});
  REQUIRE_THROWS(compiler::compile(p, {"/o.mp4", false, ""}));                 // no ASS path given
  REQUIRE_THROWS(compiler::compile(p, {"/o.mp4", false, "/tmp/it's.ass"}));    // unsafe path
  auto args = compiler::compile(p, {"/o.mp4", false, "/tmp/x.ass"});
  std::string j;
  for (auto& a : args) j += a + "\n";
  REQUIRE_THAT(j, ContainsSubstring("[v]ass=filename='/tmp/x.ass'[vout]"));
  REQUIRE_THAT(j, ContainsSubstring("-map\n[vout]\n"));

  p.set_subtitles({});
  j.clear();
  for (auto& a : compiler::compile(p, {"/o.mp4", false, ""})) j += a + "\n";
  REQUIRE_THAT(j, !ContainsSubstring("ass="));
  REQUIRE_THAT(j, ContainsSubstring("-map\n[v]\n"));
}

namespace {
namespace fs = std::filesystem;

struct Frame { int w, h; std::string gray; };

// Frame at t=1s as 8-bit grayscale.
Frame frame_of(const std::string& file, int w, int h) {
  auto r = run({"ffmpeg", "-v", "error", "-nostdin", "-ss", "1", "-i", file, "-frames:v", "1", "-f", "rawvideo", "-pix_fmt",
                "gray", "-"});
  REQUIRE(r.exit_code == 0);
  REQUIRE(r.out.size() == static_cast<size_t>(w * h));
  return {w, h, r.out};
}

// Number of clearly-changed pixels between two frames within rows [y0, y1).
int changed(const Frame& a, const Frame& b, int y0, int y1) {
  int n = 0;
  for (int y = y0; y < y1; ++y)
    for (int x = 0; x < a.w; ++x) {
      int i = y * a.w + x;
      if (std::abs(static_cast<unsigned char>(a.gray[i]) - static_cast<unsigned char>(b.gray[i])) > 40) ++n;
    }
  return n;
}
}  // namespace

TEST_CASE("end to end: subtitles are burned in at the requested position") {
  if (!ffmpeg_has_filter("ass")) SKIP("ffmpeg lacks libass; set EDITING_TOOLS_FFMPEG to one that has it");

  auto dir = fs::temp_directory_path() / "et_subs";
  fs::create_directories(dir);
  auto src = (dir / "src.mp4").string();
  REQUIRE(run({"ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i", "color=c=0x303030:s=320x240:r=10:d=3", "-f", "lavfi", "-i",
               "sine=duration=3", "-shortest", "-pix_fmt", "yuv420p", src}).exit_code == 0);

  model::Project p;
  p.width = 320; p.height = 240; p.fps = 10;
  p.add_clip(p.add_asset({"", src, 3, true, true, 320, 240}).id, 0, 3);

  render::render(p, {(dir / "plain.mp4").string(), true, ""});
  auto plain = frame_of((dir / "plain.mp4").string(), 320, 240);

  p.set_subtitles({{"", 0, 3, "Hello 你好 world"}});
  p.set_subtitle_style({{"color", "#FFFFFF"}, {"font_size", 28}});
  render::render(p, {(dir / "bottom.mp4").string(), true, ""});
  auto bottom = frame_of((dir / "bottom.mp4").string(), 320, 240);
  CHECK(changed(plain, bottom, 150, 240) > 150);  // text is in the lower part
  CHECK(changed(plain, bottom, 0, 120) < 20);     // and nowhere near the top

  p.set_subtitle_style({{"position", "top"}});
  render::render(p, {(dir / "top.mp4").string(), true, ""});
  auto top = frame_of((dir / "top.mp4").string(), 320, 240);
  CHECK(changed(plain, top, 0, 90) > 150);
  CHECK(changed(plain, top, 120, 240) < 20);

  p.set_subtitle_style({{"position", "bottom"}, {"box", true}, {"box_opacity", 1.0}, {"outline_color", "#FFFFFF"}, {"color", "#000000"}});
  render::render(p, {(dir / "box.mp4").string(), true, ""});
  auto box = frame_of((dir / "box.mp4").string(), 320, 240);
  CHECK(changed(plain, box, 150, 240) > changed(plain, bottom, 150, 240));  // a filled box changes more pixels

  fs::remove_all(dir);
}
