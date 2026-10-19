#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>

#include "process.h"
#include "tools/media.h"

TEST_CASE("probe_media reports duration and streams of a generated clip") {
  auto path = (std::filesystem::temp_directory_path() / ("et_probe_test_" + std::to_string(::getpid()) + ".mp4")).string();
  auto gen = et::run({"ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i", "testsrc=size=320x240:rate=25:duration=2",
                      "-f", "lavfi", "-i", "sine=frequency=440:duration=2", "-shortest", "-pix_fmt", "yuv420p", path});
  REQUIRE(gen.exit_code == 0);

  auto info = et::tools::probe_media(path);
  REQUIRE(info["duration_sec"].get<double>() > 1.9);
  REQUIRE(info["duration_sec"].get<double>() < 2.2);
  REQUIRE(info["streams"].size() == 2);
  REQUIRE(info["streams"][0]["width"] == 320);
  std::filesystem::remove(path);
}

TEST_CASE("probe_media rejects missing files") {
  REQUIRE_THROWS_AS(et::tools::probe_media("/no/such/file.mp4"), et::mcp::ToolError);
}
