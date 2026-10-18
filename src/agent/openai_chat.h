#pragma once
#include "agent/provider.h"

namespace et::agent {

// OpenAI-compatible /chat/completions (OpenAI, Qwen-VL, local llama.cpp / Ollama, ...), always streamed.
class OpenAiChat : public Provider {
 public:
  explicit OpenAiChat(ProviderConfig cfg);
  std::string name() const override { return "openai"; }
  Completion complete(const std::vector<Message>& messages, const std::vector<ToolSpec>& tools,
                      const CompleteOptions& opt) override;

  // Exposed for tests: the request body and the incremental SSE parser.
  json build_request(const std::vector<Message>& messages, const std::vector<ToolSpec>& tools) const;

 private:
  ProviderConfig cfg_;
};

// Accumulates streamed chat-completion chunks into a Completion.
class ChatStreamParser {
 public:
  explicit ChatStreamParser(std::function<void(const std::string&)> on_text) : on_text_(std::move(on_text)) {}
  void feed_line(const std::string& line);  // one SSE line
  Completion finish();
  bool done() const { return done_; }

 private:
  std::function<void(const std::string&)> on_text_;
  Completion c_;
  std::vector<ToolCall> calls_;
  std::vector<std::string> args_;
  bool done_ = false;
};

}  // namespace et::agent
