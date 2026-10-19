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

// The ChatGPT subscription token expires after about an hour. A long editing session can outlive it, so a 401
// triggers one token refresh and replay of the failed request (the failure happens before anything is streamed).
class CodexSessionProvider : public Provider {
 public:
  explicit CodexSessionProvider(ProviderConfig cfg) : cfg_(std::move(cfg)) {
    if (cfg_.api_key.empty()) {
      std::string err;
      auto auth = codex_valid_auth(err);
      if (!auth) throw std::invalid_argument(err);
      cfg_.api_key = auth->access;
      cfg_.account = auth->account;
    }
    rebuild();
  }
  std::string name() const override { return "codex"; }
  Completion complete(const std::vector<Message>& messages, const std::vector<ToolSpec>& tools,
                      const CompleteOptions& opt) override {
    try {
      return inner_->complete(messages, tools, opt);
    } catch (const ProviderError& e) {
      if (e.status != 401) throw;
      CodexAuth auth;
      std::string err;
      if (!load_codex_auth(auth, err) || !codex_refresh(auth, err))
        throw ProviderError(std::string(e.what()) + " (sign-in expired; run: editing-agent login)", e.status);
      cfg_.api_key = auth.access;
      cfg_.account = auth.account;
      rebuild();
      return inner_->complete(messages, tools, opt);
    }
  }

 private:
  void rebuild() { inner_ = std::make_unique<OpenAiResponses>(cfg_); }

  ProviderConfig cfg_;
  std::unique_ptr<Provider> inner_;
};

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
  if (c.kind == "codex") return std::make_unique<CodexSessionProvider>(std::move(c));
  throw std::invalid_argument("unknown provider '" + c.kind + "' (expected openai, anthropic or codex)");
}

}  // namespace et::agent
