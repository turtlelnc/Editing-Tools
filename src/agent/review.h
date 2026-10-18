#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/message.h"
#include "agent/provider.h"

namespace et::agent {

struct ReviewIssue {
  std::string severity;  // high | medium | low
  double at = -1;        // timeline seconds the problem is visible at, or -1
  std::string message;
};

// A second, independent opinion: one completion with NO tools that looks at extracted frames plus the
// timeline and reports concrete, evidence-backed problems. Used at most twice per turn by the caller.
// Never mutates the project, and treats everything in the timeline as untrusted data.
std::vector<ReviewIssue> review_edit(Provider& provider, const nlohmann::json& project, const std::string& task,
                                     const std::string& focus, int max_frames = 6);

}  // namespace et::agent
