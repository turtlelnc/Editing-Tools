#include "agent/providers.h"

#include <cstdlib>
#include <memory>
#include <stdexcept>

#include "agent/anthropic.h"
#include "agent/codex_auth.h"
#include "agent/openai_chat.h"
#include "agent/openai_responses.h"

namespace et::agent {

namespace {

std::string env(const char* name) {
  const char* v = std::getenv(name);
  return (v && *v) ? v : "";
}

}  // namespace

std::unique_ptr<Provider> make_provider(const ProviderConfig& cfg) {
  ProviderConfig c = cfg;
  if (c.kind == "openai" || c.kind.empty()) {
    c.kind = "openai";
    if (c.api_key.empty()) c.api_key = env("OPENAI_API_KEY");
    return std::make_unique<OpenAiChat>(std::move(c));
  }
  if (c.kind == "anthropic") {
    if (c.api_key.empty()) c.api_key = env("ANTHROPIC_API_KEY");
    return std::make_unique<Anthropic>(std::move(c));
  }
  if (c.kind == "codex") {
    std::string err;
    auto auth = codex_valid_auth(err);
    if (!auth) throw std::invalid_argument(err.empty() ? "not signed in for the codex provider (run: editing-agent login)"
                                                       : err);
    c.api_key = auth->access;
    c.account = auth->account;
    return std::make_unique<OpenAiResponses>(std::move(c));
  }
  throw std::invalid_argument("unknown provider '" + c.kind + "' (expected openai, anthropic or codex)");
}

}  // namespace et::agent
