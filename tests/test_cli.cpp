#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "cli/cli.h"
#include "process.h"
#include "tools/all.h"

using namespace et;
using Catch::Matchers::ContainsSubstring;
using mcp::json;
namespace fs = std::filesystem;

namespace {

struct Result {
  int code;
  std::string out, err;
  json j() const { return json::parse(out); }
  json e() const { return json::parse(err); }
};

// Every call gets a brand-new server and session, like a separate process would.
Result cli_run(const std::vector<std::string>& args, const std::string& stdin_text = "") {
  mcp::Server server("t", "0");
  auto session = std::make_shared<tools::Session>();
  tools::register_all_tools(server, session);
  std::istringstream in(stdin_text);
  std::ostringstream out, err;
  int code = cli::run(args, in, out, err, server, session);
  return {code, out.str(), err.str()};
}

struct Dir {
  fs::path path = fs::temp_directory_path() / ("et_cli_" + std::to_string(::getpid()));
  std::string project = (path / "p.json").string(), video = (path / "v.mp4").string();
  Dir() {
    fs::remove_all(path);
    fs::create_directories(path);
    REQUIRE(run({"ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i", "testsrc=size=160x120:rate=10:duration=3", "-f", "lavfi", "-i",
                 "sine=duration=3", "-shortest", "-pix_fmt", "yuv420p", video}).exit_code == 0);
  }
  ~Dir() { fs::remove_all(path); }
  Result make_project() { return cli_run({"--project", project, "create_project", "--width", "160", "--height", "120"}); }
};

}  // namespace

TEST_CASE("cli: help, version, tool list and per-tool usage") {
  auto none = cli_run({});
  CHECK(none.code == 2);  // bare invocation is a usage error, shown on stderr
  CHECK_THAT(none.err, ContainsSubstring("USAGE"));

  auto help = cli_run({"--help"});
  CHECK(help.code == 0);
  CHECK_THAT(help.out, ContainsSubstring("--project FILE"));
  CHECK_THAT(cli_run({"--version"}).out, ContainsSubstring("editing-tools"));

  auto list = cli_run({"tools"});
  CHECK_THAT(list.out, ContainsSubstring("add_clip  "));
  CHECK_THAT(list.out, ContainsSubstring("render_export  "));
  CHECK_THAT(list.out, ContainsSubstring("preview_frame  "));

  auto usage = cli_run({"help", "add_clip"});
  CHECK(usage.code == 0);
  CHECK_THAT(usage.out, ContainsSubstring("--path <string>  (required)"));
  CHECK_THAT(usage.out, ContainsSubstring("--in <number>"));
  CHECK_THAT(cli_run({"add_clip", "--help"}).out, ContainsSubstring("--path <string>"));
  CHECK(cli_run({"help", "nope"}).code == 2);
}

TEST_CASE("cli: state persists across separate invocations through the project file") {
  Dir d;
  auto created = d.make_project();
  REQUIRE(created.code == 0);
  REQUIRE(fs::exists(d.project));  // project_path defaulted from --project

  auto added = cli_run({"--project", d.project, "add_clip", "--path", d.video, "--in", "0.5", "--out", "2"});
  REQUIRE(added.code == 0);
  CHECK(added.j()["clips"][0]["in"] == 0.5);
  CHECK(added.j()["clips"][0]["out"] == 2);  // a number, not the string "2"

  auto again = cli_run({"--project", d.project, "get_timeline"});
  REQUIRE(again.code == 0);
  CHECK(again.j()["clips"].size() == 1);
  CHECK(again.j()["duration"] == 1.5);
  CHECK_FALSE(fs::exists(d.project + ".tmp"));  // atomic save leaves no temp file
}

TEST_CASE("cli: values follow the tool schema, not guesswork") {
  Dir d;
  d.make_project();
  cli_run({"--project", d.project, "add_clip", "--path", d.video});

  // A string argument stays a string even if it looks like a number.
  auto font = cli_run({"--project", d.project, "style_subtitles", "--font", "123"});
  REQUIRE(font.code == 0);
  CHECK(font.j()["subtitle_style"]["font"] == "123");

  // Booleans: bare flag = true, explicit false works.
  CHECK(cli_run({"--project", d.project, "style_subtitles", "--box"}).j()["subtitle_style"]["box"] == true);
  CHECK(cli_run({"--project", d.project, "style_subtitles", "--box", "false"}).j()["subtitle_style"]["box"] == false);

  // Negative numbers and hyphenated names.
  auto vol = cli_run({"--project", d.project, "set_audio", "--id", "c1", "--volume_db", "-6"});
  REQUIRE(vol.code == 0);
  CHECK(vol.j()["clips"][0]["audio"]["volume_db"] == -6);
  CHECK(cli_run({"--project", d.project, "set_audio", "--id", "c1", "--volume-db", "-3"}).j()["clips"][0]["audio"]["volume_db"] == -3);

  // Arrays/objects are JSON; `--name=value` form; `in`/`out` are tool params, not CLI options.
  auto subs = cli_run({"--project", d.project, "set_subtitles", "--cues", R"([{"start":0,"end":1,"text":"你好\n世界"}])"});
  REQUIRE(subs.code == 0);
  CHECK(subs.j()["subtitles"][0]["text"] == "你好\n世界");
  CHECK(cli_run({"--project", d.project, "trim_clip", "--clip_id=c1", "--in=0.25", "--out=1"}).j()["clips"][0]["in"] == 0.25);
}

TEST_CASE("cli: JSON arguments inline, from a file, from stdin; flags override") {
  Dir d;
  d.make_project();
  json base = {{"path", d.video}, {"in", 0}, {"out", 1}};

  auto inline_json = cli_run({"--project", d.project, "add_clip", base.dump(), "--out", "2"});
  REQUIRE(inline_json.code == 0);
  CHECK(inline_json.j()["clips"][0]["out"] == 2);  // the flag beat the JSON

  auto from_stdin = cli_run({"--project", d.project, "add_clip", "--args-file", "-"}, base.dump());
  REQUIRE(from_stdin.code == 0);
  CHECK(from_stdin.j()["clips"].size() == 2);

  auto file = (d.path / "args.json").string();
  std::ofstream(file) << base.dump();
  CHECK(cli_run({"--project", d.project, "add_clip", "--args-file", file}).j()["clips"].size() == 3);

  CHECK(cli_run({"--project", d.project, "add_clip", "not json"}).code == 2);
  CHECK(cli_run({"--project", d.project, "add_clip", base.dump(), "--args-file", file}).code == 2);
}

TEST_CASE("cli: errors are JSON on stderr with distinct exit codes") {
  Dir d;
  d.make_project();
  cli_run({"--project", d.project, "add_clip", "--path", d.video});

  auto unknown_flag = cli_run({"--project", d.project, "add_clip", "--pth", d.video});
  CHECK(unknown_flag.code == 2);
  CHECK_THAT(unknown_flag.e()["error"].get<std::string>(), ContainsSubstring("--path"));

  auto missing = cli_run({"--project", d.project, "add_clip"});
  CHECK(missing.code == 2);
  CHECK_THAT(missing.e()["error"].get<std::string>(), ContainsSubstring("--path"));

  auto bad_type = cli_run({"--project", d.project, "trim_clip", "--clip_id", "c1", "--in", "abc", "--out", "1"});
  CHECK(bad_type.code == 2);
  CHECK_THAT(bad_type.e()["error"].get<std::string>(), ContainsSubstring("expected a number"));

  CHECK(cli_run({"--project", d.project, "frobnicate"}).code == 2);
  CHECK(cli_run({"--project", d.project, "get_timeline", "extra"}).code == 2);

  // The tool itself rejected the edit: exit 1, project untouched.
  auto rejected = cli_run({"--project", d.project, "trim_clip", "--clip_id", "c1", "--in", "0", "--out", "99"});
  CHECK(rejected.code == 1);
  CHECK_THAT(rejected.e()["error"].get<std::string>(), ContainsSubstring("exceeds source duration"));
  CHECK(cli_run({"--project", d.project, "get_timeline"}).j()["clips"][0]["out"] == 3);

  auto no_project = cli_run({"get_timeline"});
  CHECK(no_project.code == 1);
  CHECK_THAT(no_project.e()["error"].get<std::string>(), ContainsSubstring("--project FILE"));

  auto gone = cli_run({"--project", (d.path / "nope.json").string(), "get_timeline"});
  CHECK(gone.code == 1);
  CHECK_THAT(gone.e()["error"].get<std::string>(), ContainsSubstring("not found"));
}

TEST_CASE("cli: EDITING_TOOLS_PROJECT stands in for --project; probe_media needs no project") {
  Dir d;
  d.make_project();
  setenv("EDITING_TOOLS_PROJECT", d.project.c_str(), 1);
  auto r = cli_run({"add_clip", "--path", d.video});
  unsetenv("EDITING_TOOLS_PROJECT");
  REQUIRE(r.code == 0);
  CHECK(r.j()["clips"].size() == 1);

  auto probe = cli_run({"probe_media", "--path", d.video});
  REQUIRE(probe.code == 0);
  CHECK(probe.j()["streams"].size() == 2);
}

TEST_CASE("cli: preview_frame saves the JPEG and reports its path") {
  Dir d;
  d.make_project();
  cli_run({"--project", d.project, "add_clip", "--path", d.video});

  auto read_magic = [](const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    unsigned char b[2] = {0, 0};
    f.read(reinterpret_cast<char*>(b), 2);
    return std::make_pair(b[0], b[1]);
  };

  auto def = cli_run({"--project", d.project, "preview_frame", "--time", "1", "--max_width", "80"});
  REQUIRE(def.code == 0);
  CHECK_FALSE(def.j().contains("__image"));  // no base64 blob in the output
  auto path = def.j()["image_path"].get<std::string>();
  CHECK(fs::equivalent(fs::path(path).parent_path(), d.path));  // next to the project
  CHECK(fs::path(path).filename() == "preview_1.00s.jpg");
  CHECK(read_magic(path) == std::make_pair<unsigned char, unsigned char>(0xFF, 0xD8));

  auto chosen = (d.path / "mine.jpg").string();
  auto explicit_path = cli_run({"--project", d.project, "preview_frame", "--time", "1", "--image-file", chosen});
  REQUIRE(explicit_path.code == 0);
  CHECK(explicit_path.j()["image_path"] == chosen);
  CHECK(fs::exists(chosen));
}

TEST_CASE("cli: a full edit from the command line renders a real file") {
  Dir d;
  d.make_project();
  cli_run({"--project", d.project, "add_clip", "--path", d.video, "--in", "0", "--out", "2"});
  cli_run({"--project", d.project, "color_adjust", "--saturation", "0.5", "--warmth", "0.2"});
  auto out = (d.path / "out.mp4").string();
  auto r = cli_run({"--project", d.project, "render_preview", "--output_path", out});
  REQUIRE(r.code == 0);
  CHECK(r.j()["duration_sec"].get<double>() > 1.8);
  CHECK(fs::file_size(out) > 1000);
}
