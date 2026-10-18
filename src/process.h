#pragma once
#include <functional>
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

struct StreamResult {
  int exit_code = -1;
  std::string err;  // stderr
  bool cancelled = false;
};

// Like run(), but hands stdout to `on_stdout` as it arrives instead of buffering it. Returning false from
// on_stdout, or true from should_cancel (polled about every 100 ms), terminates the child.
StreamResult run_streaming(const std::vector<std::string>& argv, const std::function<bool(const char*, size_t)>& on_stdout,
                           const std::function<bool()>& should_cancel = {});

// ffmpeg executable used for rendering: $EDITING_TOOLS_FFMPEG if set, else "ffmpeg" from PATH.
std::string ffmpeg_bin();

// True if that ffmpeg provides the named filter (e.g. "ass"). Result is cached.
bool ffmpeg_has_filter(const std::string& name);

}  // namespace et
