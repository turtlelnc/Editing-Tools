#pragma once
#include <filesystem>
#include <string>

namespace et::agent {

struct PromptContext {
  std::filesystem::path workspace;  // the folder the agent works in
  std::string project_hint;         // where the project should live (may be empty)
  bool project_open = false;        // the project is already loaded, so the agent need not open it
  std::string notes;                // optional project memory supplied by the user (--notes)
  bool can_export = true;           // false when the user asked for a dry run
};

// The editing-specific system prompt: workflow, the two kinds of time, what the tools cannot do.
std::string system_prompt(const PromptContext& ctx);

}  // namespace et::agent
