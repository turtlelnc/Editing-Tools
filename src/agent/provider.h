#pragma once
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "agent/message.h"

namespace et::agent {

struct Completion {
  std::string text;
  std::vector<ToolCall> calls;
  std::string stop_reason;
  long input_tokens = 0, output_tokens = 0;
};

struct CompleteOptions {
  std::function<void(const std::string&)> on_text_delta;    // streamed assistant text
  std::function<bool()> cancelled;                           // polled; true aborts the request
  std::function<void(const std::string&)> on_retry;         // transient failure notice
};

// A failed model call. `status` is the HTTP status when there was one.
struct ProviderError : std::runtime_error {
  long status;
  ProviderError(const std::string& msg, long status = 0) : std::runtime_error(msg), status(status) {}
};

class Provider {
 public:
  virtual ~Provider() = default;
  virtual std::string name() const = 0;
  // `messages[0]` may be a System message. Throws ProviderError.
  virtual Completion complete(const std::vector<Message>& messages, const std::vector<ToolSpec>& tools,
                              const CompleteOptions& opt) = 0;
};

struct ProviderConfig {
  std::string kind = "openai";  // openai | anthropic | codex
  std::string model;
  std::string base_url;         // empty = the provider's default
  std::string api_key;          // never logged
  std::string account;          // codex: ChatGPT account id, sent as chatgpt-account-id
  std::string instructions;     // codex: optional client originator header (defaults to editing-agent)
};

// Builds the provider named by cfg.kind. Throws std::invalid_argument for an unknown kind or missing settings.
std::unique_ptr<Provider> make_provider(const ProviderConfig& cfg);

// Shared by providers: pulls "error.message" out of a JSON error body, else returns a trimmed excerpt.
std::string error_message_from_body(const std::string& body);

}  // namespace et::agent
