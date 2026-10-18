#pragma once
#include <memory>

#include "agent/provider.h"

namespace et::agent {

// Builds the provider named by cfg.kind. Throws std::invalid_argument for an unknown kind or missing settings.
// API keys are taken from cfg.api_key or from OPENAI_API_KEY / ANTHROPIC_API_KEY; the codex provider uses the
// saved ChatGPT credentials instead (see codex_auth.h).
std::unique_ptr<Provider> make_provider(const ProviderConfig& cfg);

}  // namespace et::agent
