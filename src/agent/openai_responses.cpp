// Placeholder while the Codex/ChatGPT provider is being written. Replaced by the real implementation.
#include "agent/openai_responses.h"

namespace et::agent {

OpenAiResponses::OpenAiResponses(ProviderConfig cfg) : cfg_(std::move(cfg)) {
  if (cfg_.base_url.empty()) cfg_.base_url = "https://chatgpt.com/backend-api/codex";
  if (cfg_.model.empty()) cfg_.model = "gpt-5.1-codex";
  if (cfg_.api_key.empty()) throw std::invalid_argument("the codex provider needs a signed-in account");
}

json OpenAiResponses::build_request(const std::vector<Message>&, const std::vector<ToolSpec>&) const {
  throw std::runtime_error("codex provider: build_request is not implemented yet");
}

Completion OpenAiResponses::complete(const std::vector<Message>&, const std::vector<ToolSpec>&, const CompleteOptions&) {
  throw std::runtime_error("codex provider is not implemented yet");
}

void ResponsesStreamParser::feed_line(const std::string&) {}
Completion ResponsesStreamParser::finish() { return c_; }

}  // namespace et::agent
