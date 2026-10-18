#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace et::agent {

// ChatGPT/Codex subscription credentials, ported from Codex.cpp but stored separately:
// ~/.editing-tools/auth.json (0600), never ~/.codex/auth.json.
struct CodexAuth {
  std::string access, refresh, id, account;  // account = chatgpt account id from the id_token
  long long expires = 0;                     // unix seconds
  bool usable() const { return !access.empty() && !refresh.empty(); }
};

std::filesystem::path codex_auth_path();  // $EDITING_TOOLS_HOME/auth.json, else ~/.editing-tools/auth.json
bool load_codex_auth(CodexAuth& out, std::string& err);
bool save_codex_auth(const CodexAuth& auth, std::string& err);

// The chatgpt_account_id claim from an id_token, or "" when absent/malformed.
std::string account_from_jwt(const std::string& token);
// unix seconds now
long long epoch_now();

// Exchanges the refresh token for a fresh access token and saves it.
bool codex_refresh(CodexAuth& auth, std::string& err);

// ChatGPT device authorization: prints/announces a code and polls until the user approves it, then saves the
// credentials. `issuer` overrides https://auth.openai.com (tests point it at a mock server). `poll_ms`
// overrides the server-provided interval.
bool codex_login(std::string& err, const std::function<void(const std::string&)>& announce = {},
                 const std::string& issuer = "https://auth.openai.com", int poll_ms = 0);

// Loads the saved credentials, refreshing them when they are about to expire. Nullopt + err when not signed in.
std::optional<CodexAuth> codex_valid_auth(std::string& err);

}  // namespace et::agent
