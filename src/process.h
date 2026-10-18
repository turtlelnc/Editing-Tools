#pragma once
#include <string>
#include <vector>

namespace et {

struct ProcResult {
  int exit_code = -1;
  std::string out;  // stdout
  std::string err;  // stderr
};

// Runs argv[0] (resolved via PATH) without a shell. Captures stdout and stderr.
ProcResult run(const std::vector<std::string>& argv);

// ffmpeg executable used for rendering: $EDITING_TOOLS_FFMPEG if set, else "ffmpeg" from PATH.
std::string ffmpeg_bin();

// True if that ffmpeg provides the named filter (e.g. "ass"). Result is cached.
bool ffmpeg_has_filter(const std::string& name);

}  // namespace et
