#pragma once
#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "agent/message.h"
#include "agent/provider.h"
#include "agent/toolbox.h"

namespace et::agent {

// Progress callbacks for one turn. All are optional.
struct LoopEvents {
  std::function<void(const std::string& delta)> text_delta;            // streamed assistant prose
  std::function<void(const std::string& line)> notice;                 // one human-readable line of progress
  std::function<void(const ToolCall&)> tool_start;                     // before a tool runs
  std::function<void(const ToolCall&, const ToolResult&)> tool_done;   // after a tool ran
  std::function<void(const Message&)> on_message;                      // every message appended to the conversation
};

struct LoopConfig {
  int max_steps = 40;                  // model calls per turn before the loop stops and asks for a wrap-up
  size_t compact_after_chars = 200000;  // roughly 50k tokens; older turns are summarized above this
  size_t keep_recent_messages = 10;     // never summarize this many of the newest messages
};

struct LoopResult {
  std::string final_text;
  int steps = 0;          // model calls made
  bool hit_max_steps = false;
  bool cancelled = false;
  long input_tokens = 0, output_tokens = 0;
};

// The agent loop: model -> run the tools it asked for -> feed the results back -> repeat until it answers
// without tool calls. It never touches the network or the filesystem itself; the provider and the toolbox do.
class AgentLoop {
 public:
  AgentLoop(Provider& provider, ToolBox& toolbox, LoopConfig cfg = {});

  // Runs one turn. `messages` holds the conversation so far and ends with the new user message (or, when
  // continuing, with tool results). messages[0] may be a System message. Everything added is appended to
  // `messages` so the caller can persist or resume the session.
  LoopResult run(std::vector<Message>& messages, const LoopEvents& events = {});

  // Asks the model to stop at its next answer (polled between steps and tool calls, and by the provider).
  void cancel() { cancelled_ = true; }
  bool cancelled() const { return cancelled_.load(); }

 private:
  // Replaces old turns with a model-written summary once the conversation gets too large.
  void compact(std::vector<Message>& messages, const LoopEvents& events);
  // Keeps only the newest couple of tool-produced images in context; old previews are noise.
  static void drop_old_images(std::vector<Message>& messages);
  size_t usage_chars(const std::vector<Message>& messages) const;

  Provider& provider_;
  ToolBox& toolbox_;
  LoopConfig cfg_;
  std::atomic<bool> cancelled_{false};
};

}  // namespace et::agent
