#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "agent/provider.h"

namespace et::agent {

// Anthropic Messages API (claude-*), always streamed. Images are supported both as user input and as
// tool results (inside a tool_result block).
class Anthropic : public Provider {
 public:
  // The Messages API requires max_tokens; the agent keeps answers well below this.
  static constexpr long kMaxTokens = 8192;

  explicit Anthropic(ProviderConfig cfg);
  std::string name() const override { return "anthropic"; }
  Completion complete(const std::vector<Message>& messages, const std::vector<ToolSpec>& tools,
                      const CompleteOptions& opt) override;

  // Exposed for tests: the request body and the incremental SSE parser.
  json build_request(const std::vector<Message>& messages, const std::vector<ToolSpec>& tools) const;

  // Exposed for tests: base_url has no trailing '/' and model is never empty.
  const std::string& base_url() const { return cfg_.base_url; }
  const std::string& model() const { return cfg_.model; }

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
  // Set when the stream carried an "error" event; complete() turns it into a ProviderError.
  const std::string& error() const { return error_; }

 private:
  void append_text(const std::string& text);

  std::function<void(const std::string&)> on_text_;
  Completion c_;
  std::vector<ToolCall> calls_;
  std::vector<std::string> args_;
  std::string event_;  // last "event:" name seen, used when a data line has no "type"
  std::string error_;
  int current_block_ = -1;
  std::string current_tool_ = "";  // name of the tool_use block being streamed
  bool done_ = false;
};

}  // namespace et::agent
