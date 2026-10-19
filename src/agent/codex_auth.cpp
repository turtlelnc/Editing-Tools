#include "agent/codex_auth.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "agent/http.h"
#include "agent/provider.h"

namespace et::agent {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr const char* kOAuthClient = "app_EMoamEEZ73f0CkXaXp7hrann";
constexpr const char* kDefaultIssuer = "https://auth.openai.com";
constexpr const char* kDevicePage = "https://auth.openai.com/codex/device";
constexpr const char* kNotSignedIn = "Not signed in. Run: editing-agent login (ChatGPT subscription)";

std::string string_field(const json& j, const char* key) {
  auto it = j.find(key);
  return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

long long int_field(const json& j, const char* key, long long fallback = 0) {
  auto it = j.find(key);
  if (it == j.end()) return fallback;
  if (it->is_number_unsigned()) return static_cast<long long>(it->get<unsigned long long>());
  if (it->is_number_integer()) return it->get<long long>();
  if (it->is_string()) {
    try {
      return std::stoll(it->get<std::string>());
    } catch (...) {
    }
  }
  return fallback;
}

// The issuer is fixed for real use. Tests point it at a local mock server through this variable so the
// refresh path (which has no issuer argument in the public API) can be exercised offline.
std::string auth_issuer() {
  std::string issuer = kDefaultIssuer;
  if (const char* e = std::getenv("EDITING_TOOLS_CODEX_ISSUER"); e && *e) issuer = e;
  while (issuer.size() > 1 && issuer.back() == '/') issuer.pop_back();
  return issuer.empty() ? std::string(kDefaultIssuer) : issuer;
}

std::string form_encode(const std::string& s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 0x0F];
    }
  }
  return out;
}

// Decodes base64url (the alphabet used inside a JWT) and tolerates padding.
std::string base64url_decode(const std::string& in) {
  std::string out;
  unsigned acc = 0;
  int bits = -8;
  for (char c : in) {
    if (c == '=') break;
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '-') v = 62;
    else if (c == '_') v = 63;
    else if (c == '+') v = 62;  // tolerate standard base64 too
    else if (c == '/') v = 63;
    else return std::string();
    acc = (acc << 6) | static_cast<unsigned>(v);
    bits += 6;
    if (bits >= 0) {
      out += static_cast<char>((acc >> bits) & 0xFF);
      bits -= 8;
    }
  }
  return out;
}

bool read_text_file(const fs::path& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream buf;
  buf << f.rdbuf();
  out = buf.str();
  return true;
}

// Writes `data` into `fd`, retrying short writes and EINTR.
bool write_all(int fd, const std::string& data) {
  size_t off = 0;
  while (off < data.size()) {
    ssize_t n = ::write(fd, data.data() + off, data.size() - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

bool post_json(const std::string& url, const json& body, HttpResult& out, std::string& err) {
  HttpRequest req;
  req.url = url;
  req.headers = {"Content-Type: application/json", "Accept: application/json"};
  req.body = body.dump();
  req.timeout_sec = 60;
  out = http_post(req);
  if (!out.error.empty()) {
    err = out.error;
    return false;
  }
  return true;
}

bool post_form(const std::string& url, const std::string& body, HttpResult& out, std::string& err) {
  HttpRequest req;
  req.url = url;
  req.headers = {"Content-Type: application/x-www-form-urlencoded", "Accept: application/json"};
  req.body = body;
  req.timeout_sec = 60;
  out = http_post(req);
  if (!out.error.empty()) {
    err = out.error;
    return false;
  }
  return true;
}

std::string http_failure(const std::string& what, const HttpResult& r) {
  std::string detail = error_message_from_body(r.body);
  std::string msg = what + " (HTTP " + std::to_string(r.status) + ")";
  if (!detail.empty()) msg += ": " + detail;
  return msg;
}

// Applies an /oauth/token response to `auth`, keeping the previous refresh token and account id when the
// server omits them.
bool apply_token_response(const std::string& body, CodexAuth& auth, std::string& err) {
  json j = json::parse(body, nullptr, false);
  if (!j.is_object()) {
    err = "the OAuth token response was not valid JSON";
    return false;
  }
  const std::string access = string_field(j, "access_token");
  if (access.empty()) {
    std::string detail = string_field(j, "error_description");
    if (detail.empty()) detail = string_field(j, "error");
    err = detail.empty() ? "the OAuth token response did not contain an access token" : "OAuth token request failed: " + detail;
    return false;
  }
  auth.access = access;
  const std::string refresh = string_field(j, "refresh_token");
  if (!refresh.empty()) auth.refresh = refresh;
  const std::string id = string_field(j, "id_token");
  if (!id.empty()) {
    auth.id = id;
    auth.account = account_from_jwt(id);
  }
  long long ttl = int_field(j, "expires_in", 3600);
  auth.expires = epoch_now() + std::max<long long>(60, ttl);
  return true;
}

}  // namespace

std::filesystem::path codex_auth_path() {
  if (const char* home = std::getenv("EDITING_TOOLS_HOME"); home && *home) return fs::path(home) / "auth.json";
  if (const char* home = std::getenv("HOME"); home && *home) return fs::path(home) / ".editing-tools" / "auth.json";
  return fs::path(".editing-tools") / "auth.json";
}

bool load_codex_auth(CodexAuth& out, std::string& err) {
  const fs::path path = codex_auth_path();
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) {
    err = kNotSignedIn;
    return false;
  }
  // Refuse credentials that other users on the machine can read (same rule as Codex.cpp).
  const auto perms = fs::status(path, ec).permissions();
  if (!ec && (perms & (fs::perms::group_all | fs::perms::others_all)) != fs::perms::none) {
    err = "the saved ChatGPT credentials at " + path.string() + " are readable by other users; run: chmod 600 " +
          path.string();
    return false;
  }
  std::string text;
  if (!read_text_file(path, text)) {
    err = "cannot read the saved ChatGPT credentials at " + path.string();
    return false;
  }
  json j = json::parse(text, nullptr, false);
  if (!j.is_object()) {
    err = "the saved ChatGPT credentials are not valid JSON; run: editing-agent login";
    return false;
  }
  CodexAuth auth;
  auth.access = string_field(j, "access_token");
  auth.refresh = string_field(j, "refresh_token");
  auth.id = string_field(j, "id_token");
  auth.account = string_field(j, "account_id");
  if (auth.account.empty() && !auth.id.empty()) auth.account = account_from_jwt(auth.id);
  auth.expires = int_field(j, "expires_at");
  if (!auth.usable()) {
    err = "the saved ChatGPT credentials are incomplete; run: editing-agent login";
    return false;
  }
  out = std::move(auth);
  err.clear();
  return true;
}

bool save_codex_auth(const CodexAuth& auth, std::string& err) {
  const fs::path path = codex_auth_path();
  const fs::path dir = path.parent_path();
  if (dir.empty()) {
    err = "cannot determine the credential directory";
    return false;
  }
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    err = "cannot create the credential directory " + dir.string() + ": " + ec.message();
    return false;
  }
  // 0700 for the directory; ignore a filesystem that refuses chmod as long as the file itself is 0600.
  fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);

  const json doc = {{"access_token", auth.access},
                    {"refresh_token", auth.refresh},
                    {"id_token", auth.id},
                    {"account_id", auth.account},
                    {"expires_at", auth.expires}};
  const std::string data = doc.dump();

  static std::atomic<unsigned> counter{0};
  fs::path tmp;
  int fd = -1;
  for (int attempt = 0; attempt < 8 && fd < 0; ++attempt) {
    tmp = dir / (".auth.json.tmp." + std::to_string(static_cast<long long>(::getpid())) + "." + std::to_string(counter++));
    fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);  // O_EXCL + 0600: never exposes a partial secret
  }
  if (fd < 0) {
    err = "cannot create a temporary credential file in " + dir.string();
    return false;
  }
  if (!write_all(fd, data)) {
    ::close(fd);
    fs::remove(tmp, ec);
    err = "cannot write the ChatGPT credentials";
    return false;
  }
  if (::close(fd) != 0) {
    fs::remove(tmp, ec);
    err = "cannot write the ChatGPT credentials";
    return false;
  }
  fs::rename(tmp, path, ec);  // atomic replace within the same directory
  if (ec) {
    fs::remove(tmp, ec);
    err = "cannot replace the credential file " + path.string() + ": " + ec.message();
    return false;
  }
  err.clear();
  return true;
}

std::string account_from_jwt(const std::string& token) {
  const size_t first = token.find('.');
  if (first == std::string::npos) return std::string();
  const size_t second = token.find('.', first + 1);
  if (second == std::string::npos || second == first + 1) return std::string();
  const std::string payload = base64url_decode(token.substr(first + 1, second - first - 1));
  if (payload.empty()) return std::string();
  json j = json::parse(payload, nullptr, false);
  if (!j.is_object()) return std::string();
  auto auth = j.find("https://api.openai.com/auth");
  if (auth == j.end() || !auth->is_object()) return std::string();
  auto id = auth->find("chatgpt_account_id");
  if (id == auth->end() || !id->is_string()) return std::string();
  return id->get<std::string>();
}

long long epoch_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool codex_refresh(CodexAuth& auth, std::string& err) {
  if (auth.refresh.empty()) {
    err = "no ChatGPT refresh token is saved; run: editing-agent login (ChatGPT subscription)";
    return false;
  }
  const std::string body = "grant_type=refresh_token&client_id=" + form_encode(kOAuthClient) +
                           "&refresh_token=" + form_encode(auth.refresh);
  HttpResult r;
  std::string transport;
  if (!post_form(auth_issuer() + "/oauth/token", body, r, transport)) {
    err = "token refresh failed: " + transport;
    return false;
  }
  if (r.status != 200) {
    err = http_failure("token refresh failed", r);
    return false;
  }
  if (!apply_token_response(r.body, auth, err)) return false;
  return save_codex_auth(auth, err);
}

bool codex_login(std::string& err, const std::function<void(const std::string&)>& announce, const std::string& issuer,
                 int poll_ms) {
  std::string base = issuer.empty() ? auth_issuer() : issuer;
  while (base.size() > 1 && base.back() == '/') base.pop_back();

  HttpResult r;
  std::string transport;
  if (!post_json(base + "/api/accounts/deviceauth/usercode", json{{"client_id", kOAuthClient}}, r, transport)) {
    err = "device authorization is unavailable: " + transport;
    return false;
  }
  if (r.status != 200) {
    err = http_failure("device authorization is unavailable", r);
    return false;
  }
  const json device = json::parse(r.body, nullptr, false);
  if (!device.is_object()) {
    err = "the device authorization response was not valid JSON";
    return false;
  }
  const std::string device_auth_id = string_field(device, "device_auth_id");
  std::string user_code = string_field(device, "user_code");
  if (user_code.empty()) user_code = string_field(device, "usercode");
  if (device_auth_id.empty() || user_code.empty()) {
    err = "the device authorization response did not contain a code";
    return false;
  }

  int interval_ms = 5000;
  {
    long long seconds = int_field(device, "interval", 5);
    seconds = std::max<long long>(1, std::min<long long>(30, seconds));
    interval_ms = static_cast<int>(seconds * 1000);
  }
  if (poll_ms > 0) interval_ms = std::max(1, poll_ms);  // tests use a few milliseconds

  const std::string notice = "Open " + std::string(kDevicePage) + " and enter: " + user_code +
                             "\nOnly approve this code if you started this login. Waiting up to 15 minutes... "
                             "(Ctrl+C cancels)";
  if (announce) {
    announce(notice);
  } else {
    std::cout << notice << "\n" << std::flush;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(15);
  while (std::chrono::steady_clock::now() < deadline) {
    HttpResult poll;
    if (!post_json(base + "/api/accounts/deviceauth/token",
                   json{{"device_auth_id", device_auth_id}, {"user_code", user_code}}, poll, transport)) {
      err = "device authorization polling failed: " + transport;
      return false;
    }
    if (poll.status == 200) {
      const json data = json::parse(poll.body, nullptr, false);
      if (!data.is_object()) {
        err = "the authorization response was not valid JSON";
        return false;
      }
      const std::string auth_code = string_field(data, "authorization_code");
      const std::string verifier = string_field(data, "code_verifier");
      if (auth_code.empty() || verifier.empty()) {
        err = "the authorization response was incomplete";
        return false;
      }
      const std::string form = "grant_type=authorization_code&client_id=" + form_encode(kOAuthClient) +
                               "&code=" + form_encode(auth_code) + "&code_verifier=" + form_encode(verifier) +
                               "&redirect_uri=" + form_encode(base + "/deviceauth/callback");
      HttpResult token;
      if (!post_form(base + "/oauth/token", form, token, transport)) {
        err = "token exchange failed: " + transport;
        return false;
      }
      if (token.status != 200) {
        err = http_failure("token exchange failed", token);
        return false;
      }
      CodexAuth auth;
      if (!apply_token_response(token.body, auth, err)) return false;
      if (auth.refresh.empty()) {
        err = "the OAuth token response did not contain a refresh token";
        return false;
      }
      return save_codex_auth(auth, err);
    }
    if (poll.status != 403 && poll.status != 404) {
      err = http_failure("device authorization failed", poll);
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }
  err = "device authorization timed out after 15 minutes";
  return false;
}

std::optional<CodexAuth> codex_valid_auth(std::string& err) {
  CodexAuth auth;
  if (!load_codex_auth(auth, err)) return std::nullopt;
  if (auth.expires <= epoch_now() + 90) {
    std::string refresh_err;
    if (!codex_refresh(auth, refresh_err)) {
      err = refresh_err;
      return std::nullopt;
    }
  }
  err.clear();
  return auth;
}

}  // namespace et::agent
