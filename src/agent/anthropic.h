#pragma once
#include "agent/provider.h"

namespace et::agent {

// Anthropic Messages API (claude-*), always streamed. Images are supported both as user input and as
// tool results (inside a tool_result block).
class Anthropic : public Provider {
 public:
  explicit Anthropic(ProviderConfig cfg);
  std::string name() const override { return "anthropic"; }
  Completion complete(const std::vector<Message>& messages, const std::vector<ToolSpec>& tools,
                      const CompleteOptions& opt) override;

  // Exposed for tests: the request body and the incremental SSE parser.
  json build_request(const std::vector<Message>& messages, const std::vector<ToolSpec>& tools) const;

 private:
  ProviderConfig cfg_;
};

// Accumulates streamed Anthropic events (content_block_start / _delta / message_delta / message_stop) into a
// Completion. feed_line() takes the raw SSE lines, including the "event:" lines.
class AnthropicStreamParser {
 public:
  explicit AnthropicStreamParser(std::function<void(const std::string&)> on_text) : on_text_(std::move(on_text)) {}
  void feed_line(const std::string& line);
  Completion finish();
  bool done() const { return done_; }

 private:
  std::function<void(const std::string&)> on_text_;
  Completion c_;
  std::vector<ToolCall> calls_;
  std::vector<std::string> args_;
  int current_block_ = -1;
  std::string current_tool_ = "";  // name of the tool_use block being streamed
  bool done_ = false;
};

}  // namespace et::agent
