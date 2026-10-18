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

}  // namespace et
