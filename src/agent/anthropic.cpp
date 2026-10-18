// Placeholder while the Anthropic provider is being written. Replaced by the real implementation.
#include "agent/anthropic.h"

namespace et::agent {

Anthropic::Anthropic(ProviderConfig cfg) : cfg_(std::move(cfg)) {
  if (cfg_.base_url.empty()) cfg_.base_url = "https://api.anthropic.com";
  if (cfg_.model.empty()) cfg_.model = "claude-sonnet-5-5";
  if (cfg_.api_key.empty()) throw std::invalid_argument("the anthropic provider needs ANTHROPIC_API_KEY");
}

json Anthropic::build_request(const std::vector<Message>&, const std::vector<ToolSpec>&) const {
  throw std::runtime_error("anthropic provider: build_request is not implemented yet");
}

Completion Anthropic::complete(const std::vector<Message>&, const std::vector<ToolSpec>&, const CompleteOptions&) {
  throw std::runtime_error("anthropic provider is not implemented yet");
}

void AnthropicStreamParser::feed_line(const std::string&) {}
Completion AnthropicStreamParser::finish() { return c_; }

}  // namespace et::agent
