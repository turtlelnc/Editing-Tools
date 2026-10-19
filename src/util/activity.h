#pragma once
#include <functional>
#include <string>
#include <utility>

namespace et::activity {

// Long-running work (renders, analysis, transcription) reports through a thread-local hook, so tools never need
// to know who is watching. The agent installs a hook per tool call; the CLI and MCP server leave it unset and the
// work just runs silently as before.
using ProgressFn = std::function<void(const std::string&)>;
using CancelFn = std::function<bool()>;

struct Hooks {
  ProgressFn progress;
  CancelFn cancelled;
};

inline Hooks& current() {
  thread_local Hooks hooks;
  return hooks;
}

inline bool watching() { return static_cast<bool>(current().progress) || static_cast<bool>(current().cancelled); }

inline void report(const std::string& line) {
  if (current().progress) current().progress(line);
}

inline bool cancelled() { return current().cancelled && current().cancelled(); }

// Installs hooks for the duration of a scope; nested scopes restore the previous ones.
class Scope {
 public:
  Scope(ProgressFn progress, CancelFn cancelled) : previous_(current()) {
    current().progress = std::move(progress);
    current().cancelled = std::move(cancelled);
  }
  ~Scope() { current() = previous_; }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  Hooks previous_;
};

// Test helper: a scope that records reported lines.
class Recorder {
 public:
  explicit Recorder(CancelFn cancelled = {})
      : scope_([this](const std::string& line) { lines.push_back(line); }, std::move(cancelled)) {}
  std::vector<std::string> lines;

 private:
  Scope scope_;
};

}  // namespace et::activity
