#pragma once
#include "agent/provider.h"

namespace et::agent {

// OpenAI Responses API (POST /v1/responses), always streamed. Used for the ChatGPT/Codex subscription
// backend (kind "codex"), whose endpoint requires the account id header and cannot use Chat Completions.
class OpenAiResponses : public Provider {
 public:
  explicit OpenAiResponses(ProviderConfig cfg);
  std::string name() const override { return "codex"; }
  Completion complete(const std::vector<Message>& messages, const std::vector<ToolSpec>& tools,
                      const CompleteOptions& opt) override;

  // Exposed for tests: the request body and the incremental SSE parser.
  json build_request(const std::vector<Message>& messages, const std::vector<ToolSpec>& tools) const;

 private:
  ProviderConfig cfg_;
};

// Accumulates streamed Responses API events (response.output_item.*, response.output_text.delta,
// response.function_call_arguments.delta, response.completed) into a Completion.
class ResponsesStreamParser {
 public:
  explicit ResponsesStreamParser(std::function<void(const std::string&)> on_text) : on_text_(std::move(on_text)) {}
  void feed_line(const std::string& line);
  Completion finish();
  bool done() const { return done_; }
  // The message from a response.failed / error event, empty when the stream did not fail.
  const std::string& error() const { return error_; }

 private:
  std::function<void(const std::string&)> on_text_;
  Completion c_;
  std::vector<ToolCall> calls_;
  std::vector<std::string> args_;
  std::vector<std::string> ids_;
  std::vector<std::string> names_;
  bool done_ = false;
  std::string error_;
};

}  // namespace et::agent
