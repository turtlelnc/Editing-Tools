// Placeholder while ChatGPT device sign-in is being written. Replaced by the real implementation.
#include "agent/codex_auth.h"

#include <chrono>

namespace et::agent {

std::filesystem::path codex_auth_path() {
  const char* home = std::getenv("HOME");
  return (home && *home ? std::filesystem::path(home) : std::filesystem::path(".")) / ".editing-tools" / "auth.json";
}

bool load_codex_auth(CodexAuth&, std::string& err) {
  err = "ChatGPT sign-in is not implemented yet";
  return false;
}

bool save_codex_auth(const CodexAuth&, std::string& err) {
  err = "ChatGPT sign-in is not implemented yet";
  return false;
}

std::string account_from_jwt(const std::string&) { return ""; }

long long epoch_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool codex_refresh(CodexAuth&, std::string& err) {
  err = "ChatGPT sign-in is not implemented yet";
  return false;
}

bool codex_login(std::string& err, const std::function<void(const std::string&)>&, const std::string&, int) {
  err = "ChatGPT sign-in is not implemented yet";
  return false;
}

std::optional<CodexAuth> codex_valid_auth(std::string& err) {
  err = "Not signed in. Run: editing-agent login (ChatGPT subscription)";
  return std::nullopt;
}

}  // namespace et::agent
