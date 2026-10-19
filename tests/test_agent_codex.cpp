// Codex/ChatGPT subscription provider and device sign-in tests. Everything runs against the local
// scripted MockServer; no request ever leaves the machine.
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include "agent/codex_auth.h"
#include "agent/openai_responses.h"
#include "agent/provider.h"
#include "mock_http.h"
#include "util/base64.h"

using namespace et;
using Catch::Matchers::ContainsSubstring;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

int next_suffix() {
  static std::atomic<int> n{0};
  return n++;
}

// A throw-away $EDITING_TOOLS_HOME so auth.json never touches the real one.
struct TempHome {
  fs::path path;
  TempHome() {
    path = fs::temp_directory_path() /
           ("et_codex_home_" + std::to_string(::getpid()) + "_" + std::to_string(next_suffix()));
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path, ec);
    if (ec) throw std::runtime_error("cannot create temp home " + path.string());
  }
  ~TempHome() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  TempHome(const TempHome&) = delete;
  TempHome& operator=(const TempHome&) = delete;
};

// Sets an environment variable for the duration of a test and restores the previous value afterwards.
struct EnvGuard {
  std::string name, old;
  bool had = false;
  EnvGuard(std::string n, const std::string& value) : name(std::move(n)) {
    if (const char* v = std::getenv(name.c_str())) {
      had = true;
      old = v;
    }
    setenv(name.c_str(), value.c_str(), 1);
  }
  ~EnvGuard() {
    if (had) {
      setenv(name.c_str(), old.c_str(), 1);
    } else {
      unsetenv(name.c_str());
    }
  }
  EnvGuard(const EnvGuard&) = delete;
  EnvGuard& operator=(const EnvGuard&) = delete;
};

std::string base64url(const std::string& raw) {
  std::string s = util::base64(raw);
  for (char& c : s) {
    if (c == '+') c = '-';
    else if (c == '/') c = '_';
  }
  while (!s.empty() && s.back() == '=') s.pop_back();
  return s;
}

// A JWT shaped like the one auth.openai.com returns; only the payload matters to us.
std::string make_id_token(const std::string& account) {
  const json header = {{"alg", "none"}, {"typ", "JWT"}};
  const json payload = {{"https://api.openai.com/auth", {{"chatgpt_account_id", account}}}, {"sub", "user-1"}};
  return base64url(header.dump()) + "." + base64url(payload.dump()) + ".signature";
}

int file_mode(const fs::path& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return -1;
  return static_cast<int>(st.st_mode & 0777);
}

std::string sse_events(const std::vector<json>& events) {
  std::string out;
  for (const json& event : events) out += "data: " + event.dump() + "\n\n";
  return out;
}

std::unique_ptr<agent::OpenAiResponses> make_codex(test::MockServer& server) {
  agent::ProviderConfig cfg;
  cfg.kind = "codex";
  cfg.model = "gpt-5.1-codex";
  cfg.api_key = "token";
  cfg.account = "acct";
  cfg.base_url = server.base_url();
  return std::make_unique<agent::OpenAiResponses>(cfg);
}

std::vector<agent::Message> simple_conversation() {
  std::vector<agent::Message> messages;
  agent::Message user;
  user.role = agent::Message::Role::User;
  user.text = "do something";
  messages.push_back(std::move(user));
  return messages;
}

const std::vector<agent::ToolSpec> kNoTools;

}  // namespace

TEST_CASE("codex auth: JWT account id, saved credentials and refresh") {
  TempHome home;
  EnvGuard home_env("EDITING_TOOLS_HOME", home.path.string());

  const std::string id_token = make_id_token("acct_123");
  CHECK(agent::account_from_jwt(id_token) == "acct_123");
  CHECK(agent::account_from_jwt("not-a-jwt") == "");
  CHECK(agent::account_from_jwt("header.!!!!.signature") == "");
  CHECK(agent::account_from_jwt(make_id_token("") ) == "");

  agent::CodexAuth saved;
  saved.access = "old-access";
  saved.refresh = "old-refresh";
  saved.id = id_token;
  saved.account = agent::account_from_jwt(id_token);
  saved.expires = agent::epoch_now() + 3600;
  std::string err = "unset";
  REQUIRE(agent::save_codex_auth(saved, err));
  CHECK(err.empty());

  const fs::path auth_file = home.path / "auth.json";
  REQUIRE(fs::is_regular_file(auth_file));
  CHECK(file_mode(auth_file) == 0600);
  CHECK(file_mode(home.path) == 0700);

  agent::CodexAuth loaded;
  REQUIRE(agent::load_codex_auth(loaded, err));
  CHECK(err.empty());
  CHECK(loaded.access == "old-access");
  CHECK(loaded.refresh == "old-refresh");
  CHECK(loaded.id == id_token);
  CHECK(loaded.account == "acct_123");
  CHECK(loaded.expires == saved.expires);

  // A credential that is about to expire is refreshed through /oauth/token.
  test::MockServer server;
  EnvGuard issuer_env("EDITING_TOOLS_CODEX_ISSUER", server.base_url());

  agent::CodexAuth stale = saved;
  stale.access = "stale-access";
  stale.refresh = "refresh-old";
  stale.expires = agent::epoch_now() + 10;
  REQUIRE(agent::save_codex_auth(stale, err));

  const std::string fresh_id_token = make_id_token("acct_fresh");
  server.enqueue(json{{"access_token", "new-access"},
                      {"refresh_token", "new-refresh"},
                      {"id_token", fresh_id_token},
                      {"expires_in", 3600}}
                     .dump());

  std::string valid_err = "unset";
  const auto valid = agent::codex_valid_auth(valid_err);
  REQUIRE(valid.has_value());
  CHECK(valid_err.empty());
  CHECK(valid->access == "new-access");
  CHECK(valid->refresh == "new-refresh");
  CHECK(valid->account == "acct_fresh");
  CHECK(valid->expires > agent::epoch_now() + 3000);

  REQUIRE(server.request_count() == 1);
  const test::MockServer::Request refresh_req = server.request(0);
  CHECK(refresh_req.path == "/oauth/token");
  CHECK_THAT(refresh_req.body, ContainsSubstring("grant_type=refresh_token"));
  CHECK_THAT(refresh_req.body, ContainsSubstring("refresh_token=refresh-old"));
  CHECK_THAT(refresh_req.body, ContainsSubstring("client_id=app_EMoamEEZ73f0CkXaXp7hrann"));

  agent::CodexAuth reloaded;
  REQUIRE(agent::load_codex_auth(reloaded, err));
  CHECK(reloaded.access == "new-access");
  CHECK(reloaded.refresh == "new-refresh");
  CHECK(reloaded.account == "acct_fresh");

  // Missing credentials produce the sign-in hint.
  std::error_code ec;
  fs::remove(auth_file, ec);
  CHECK_FALSE(agent::load_codex_auth(reloaded, err));
  CHECK(err == "Not signed in. Run: editing-agent login (ChatGPT subscription)");
}

TEST_CASE("codex auth: device login against a mock issuer") {
  TempHome home;
  EnvGuard home_env("EDITING_TOOLS_HOME", home.path.string());
  test::MockServer server;

  const std::string id_token = make_id_token("acct_login");
  server.enqueue(json{{"device_auth_id", "device-1"}, {"user_code", "ABCD-1234"}, {"interval", 1}}.dump());
  server.enqueue(json{{"error", "authorization_pending"}}.dump(), 403);
  server.enqueue(json{{"authorization_code", "auth-code-1"}, {"code_verifier", "verifier-1"}}.dump());
  server.enqueue(json{{"access_token", "access-login"},
                      {"refresh_token", "refresh-login"},
                      {"id_token", id_token},
                      {"expires_in", 3600}}
                     .dump());

  std::vector<std::string> notices;
  std::string err = "unset";
  const bool ok = agent::codex_login(
      err, [&](const std::string& line) { notices.push_back(line); }, server.base_url(), 5);
  REQUIRE(ok);
  CHECK(err.empty());
  REQUIRE(notices.size() == 1);
  CHECK_THAT(notices[0], ContainsSubstring("https://auth.openai.com/codex/device"));
  CHECK_THAT(notices[0], ContainsSubstring("ABCD-1234"));

  REQUIRE(server.request_count() == 4);
  const test::MockServer::Request usercode_req = server.request(0);
  CHECK(usercode_req.path == "/api/accounts/deviceauth/usercode");
  const json usercode_body = json::parse(usercode_req.body);
  CHECK(usercode_body["client_id"] == "app_EMoamEEZ73f0CkXaXp7hrann");

  const test::MockServer::Request first_poll = server.request(1);
  CHECK(first_poll.path == "/api/accounts/deviceauth/token");
  const json first_poll_body = json::parse(first_poll.body);
  CHECK(first_poll_body["device_auth_id"] == "device-1");
  CHECK(first_poll_body["user_code"] == "ABCD-1234");

  CHECK(server.request(2).path == "/api/accounts/deviceauth/token");
  const test::MockServer::Request token_req = server.request(3);
  CHECK(token_req.path == "/oauth/token");
  CHECK_THAT(token_req.body, ContainsSubstring("grant_type=authorization_code"));
  CHECK_THAT(token_req.body, ContainsSubstring("code=auth-code-1"));
  CHECK_THAT(token_req.body, ContainsSubstring("code_verifier=verifier-1"));
  CHECK_THAT(token_req.body, ContainsSubstring("redirect_uri=http%3A%2F%2F127.0.0.1"));

  agent::CodexAuth saved;
  REQUIRE(agent::load_codex_auth(saved, err));
  CHECK(saved.access == "access-login");
  CHECK(saved.refresh == "refresh-login");
  CHECK(saved.id == id_token);
  CHECK(saved.account == "acct_login");
  CHECK(saved.expires > agent::epoch_now() + 3000);
  CHECK(file_mode(home.path / "auth.json") == 0600);
}

TEST_CASE("codex provider: request shape and headers") {
  test::MockServer server;
  auto provider = make_codex(server);

  std::vector<agent::Message> messages;
  {
    agent::Message system;
    system.role = agent::Message::Role::System;
    system.text = "You are an editor.";
    messages.push_back(system);
    system.text = "Be concise.";
    messages.push_back(system);
  }
  {
    agent::Message user;
    user.role = agent::Message::Role::User;
    user.text = "look at this frame";
    user.images.push_back({"image/png", "AAAA"});
    messages.push_back(std::move(user));
  }
  {
    agent::Message assistant;
    assistant.role = agent::Message::Role::Assistant;
    assistant.text = "ok";
    assistant.calls.push_back({"call_1", "add_clip", {{"path", "a.mp4"}}});
    assistant.calls.push_back({"call_2", "preview_frame", {{"time", 1.0}}});
    messages.push_back(std::move(assistant));
  }
  {
    agent::Message tool;
    tool.role = agent::Message::Role::Tool;
    tool.call_id = "call_2";
    tool.tool_name = "preview_frame";
    tool.text = "frame ready";
    tool.images.push_back({"image/jpeg", "BBBB"});
    messages.push_back(std::move(tool));
  }
  {
    agent::Message user;
    user.role = agent::Message::Role::User;
    user.text = "next";
    messages.push_back(std::move(user));
  }

  std::vector<agent::ToolSpec> tools;
  tools.push_back({"get_timeline", "Read the timeline", json{{"type", "object"}, {"properties", json::object()}}});
  tools.push_back(
      {"preview_frame", "Show a frame", json{{"type", "object"}, {"properties", {{"time", {{"type", "number"}}}}}}});

  const json body = provider->build_request(messages, tools);
  CHECK(body["model"] == "gpt-5.1-codex");
  CHECK(body["instructions"] == "You are an editor.\n\nBe concise.");
  CHECK(body["stream"] == true);
  CHECK(body["store"] == false);
  CHECK(body["tool_choice"] == "auto");
  CHECK(body["parallel_tool_calls"] == true);
  CHECK_FALSE(body.contains("reasoning"));

  const json& input = body["input"];
  REQUIRE(input.is_array());
  REQUIRE(input.size() == 7);

  // The user turn keeps its text and image as content parts.
  CHECK(input[0]["role"] == "user");
  REQUIRE(input[0]["content"].size() == 2);
  CHECK(input[0]["content"][0]["type"] == "input_text");
  CHECK(input[0]["content"][0]["text"] == "look at this frame");
  CHECK(input[0]["content"][1]["type"] == "input_image");
  CHECK(input[0]["content"][1]["image_url"] == "data:image/png;base64,AAAA");

  // Assistant text is a message item; each tool call is its own function_call item.
  CHECK(input[1]["type"] == "message");
  CHECK(input[1]["role"] == "assistant");
  CHECK(input[1]["content"][0]["type"] == "output_text");
  CHECK(input[1]["content"][0]["text"] == "ok");

  REQUIRE(input[2]["type"] == "function_call");
  CHECK(input[2]["call_id"] == "call_1");
  CHECK(input[2]["name"] == "add_clip");
  CHECK(json::parse(input[2]["arguments"].get<std::string>())["path"] == "a.mp4");

  REQUIRE(input[3]["type"] == "function_call");
  CHECK(input[3]["call_id"] == "call_2");
  CHECK(input[3]["name"] == "preview_frame");

  // The tool result is a function_call_output; its image cannot travel there, so a following user
  // message carries the input_image part.
  REQUIRE(input[4]["type"] == "function_call_output");
  CHECK(input[4]["call_id"] == "call_2");
  CHECK(input[4]["output"] == "frame ready");
  REQUIRE(input[5]["role"] == "user");
  REQUIRE(input[5]["content"].size() == 2);
  CHECK(input[5]["content"][0]["type"] == "input_text");
  CHECK(input[5]["content"][0]["text"] == "Images returned by the tool call above");
  CHECK(input[5]["content"][1]["type"] == "input_image");
  CHECK(input[5]["content"][1]["image_url"] == "data:image/jpeg;base64,BBBB");

  CHECK(input[6]["role"] == "user");
  CHECK(input[6]["content"][0]["text"] == "next");

  // Tools use the flat Responses format (no nested "function" object).
  REQUIRE(body["tools"].size() == 2);
  CHECK(body["tools"][0]["type"] == "function");
  CHECK(body["tools"][0]["name"] == "get_timeline");
  CHECK(body["tools"][0]["description"] == "Read the timeline");
  CHECK(body["tools"][0]["parameters"]["type"] == "object");
  CHECK_FALSE(body["tools"][0].contains("function"));
  CHECK(body["tools"][1]["name"] == "preview_frame");

  // A real call goes to <base_url>/responses with the subscription headers.
  const std::vector<json> ready_events = {
      {{"type", "response.output_text.delta"}, {"delta", "ready"}},
      {{"type", "response.completed"}, {"response", {{"usage", {{"input_tokens", 1}, {"output_tokens", 1}}}}}}};
  server.enqueue(sse_events(ready_events), 200, "text/event-stream");
  const agent::Completion completion = provider->complete(messages, tools, agent::CompleteOptions{});
  CHECK(completion.text == "ready");

  REQUIRE(server.request_count() == 1);
  const test::MockServer::Request request = server.request(0);
  CHECK(request.path == "/responses");
  CHECK(request.header("authorization") == "Bearer token");
  CHECK(request.header("chatgpt-account-id") == "acct");
  CHECK(request.header("openai-beta") == "responses=experimental");
  CHECK(request.header("originator") == "editing-agent");
  CHECK_THAT(request.header("content-type"), ContainsSubstring("application/json"));
  CHECK_THAT(request.header("accept"), ContainsSubstring("text/event-stream"));
  CHECK(json::parse(request.body) == body);
}

TEST_CASE("codex provider: streams text, tool arguments and usage") {
  test::MockServer server;
  auto provider = make_codex(server);

  std::vector<json> events;
  events.push_back({{"type", "response.output_item.added"},
                    {"item",
                     {{"type", "function_call"},
                      {"id", "fc_1"},
                      {"call_id", "call_abc"},
                      {"name", "add_clip"},
                      {"arguments", ""}}}});
  events.push_back({{"type", "response.function_call_arguments.delta"},
                    {"item_id", "fc_1"},
                    {"delta", "{\"path\":\"a.m"}});
  events.push_back({{"type", "response.function_call_arguments.delta"},
                    {"item_id", "fc_1"},
                    {"delta", "p4\"}"}});
  // The done event's final arguments win over the accumulated deltas.
  events.push_back({{"type", "response.output_item.done"},
                    {"item",
                     {{"type", "function_call"},
                      {"id", "fc_1"},
                      {"call_id", "call_abc"},
                      {"name", "add_clip"},
                      {"arguments", "{\"path\":\"final.mp4\"}"}}}});
  // A call whose arguments never parse and that only has an item id.
  events.push_back({{"type", "response.output_item.added"},
                    {"item", {{"type", "function_call"}, {"id", "fc_2"}, {"name", "render_export"}, {"arguments", ""}}}});
  events.push_back(
      {{"type", "response.function_call_arguments.delta"}, {"item_id", "fc_2"}, {"delta", "not json"}});
  events.push_back({{"type", "response.output_text.delta"}, {"delta", "Added"}});
  events.push_back({{"type", "response.output_text.delta"}, {"delta", " the clip."}});
  events.push_back({{"type", "response.completed"},
                    {"response", {{"usage", {{"input_tokens", 12}, {"output_tokens", 7}}}}}});
  server.enqueue(sse_events(events), 200, "text/event-stream");

  std::string streamed;
  agent::CompleteOptions opt;
  opt.on_text_delta = [&](const std::string& delta) { streamed += delta; };
  const agent::Completion completion = provider->complete(simple_conversation(), kNoTools, opt);

  CHECK(streamed == "Added the clip.");
  CHECK(completion.text == "Added the clip.");
  REQUIRE(completion.calls.size() == 2);
  CHECK(completion.calls[0].id == "call_abc");
  CHECK(completion.calls[0].name == "add_clip");
  CHECK(completion.calls[0].arguments["path"] == "final.mp4");
  CHECK(completion.calls[1].id == "fc_2");
  CHECK(completion.calls[1].name == "render_export");
  CHECK(completion.calls[1].arguments["__invalid_arguments"] == "not json");
  CHECK(completion.input_tokens == 12);
  CHECK(completion.output_tokens == 7);
}

TEST_CASE("codex provider: missing call ids fall back to call_<n>") {
  agent::ResponsesStreamParser parser(nullptr);
  parser.feed_line("data: " + json{{"type", "response.output_item.added"},
                                   {"item", {{"type", "function_call"}, {"name", "get_timeline"}, {"arguments", "{}"}}}}
                                      .dump());
  parser.feed_line("data: " + json{{"type", "response.output_item.done"},
                                   {"item", {{"type", "function_call"}, {"name", "get_timeline"}, {"arguments", "{}"}}}}
                                      .dump());
  const agent::Completion completion = parser.finish();
  REQUIRE(completion.calls.size() == 1);
  CHECK(completion.calls[0].id == "call_0");
  CHECK(completion.calls[0].name == "get_timeline");
}

TEST_CASE("codex provider: a 401 becomes a ProviderError") {
  test::MockServer server;
  auto provider = make_codex(server);
  server.enqueue(json{{"error", {{"message", "invalid access token"}}}}.dump(), 401);

  try {
    provider->complete(simple_conversation(), kNoTools, agent::CompleteOptions{});
    FAIL("expected a ProviderError");
  } catch (const agent::ProviderError& e) {
    CHECK(e.status == 401);
    CHECK_THAT(e.what(), ContainsSubstring("HTTP 401"));
    CHECK_THAT(e.what(), ContainsSubstring("invalid access token"));
  }
  CHECK(server.request_count() == 1);
}

TEST_CASE("codex provider: salvages a plain JSON response") {
  test::MockServer server;
  auto provider = make_codex(server);

  const json document = {
      {"id", "resp_1"},
      {"output",
       json::array({{{"type", "message"},
                     {"role", "assistant"},
                     {"content", json::array({{{"type", "output_text"}, {"text", "plain answer"}}})}},
                    {{"type", "function_call"},
                     {"call_id", "call_x"},
                     {"name", "get_timeline"},
                     {"arguments", "{\"detail\":true}"}}})},
      {"usage", {{"input_tokens", 3}, {"output_tokens", 4}}}};
  server.enqueue(document.dump(), 200, "application/json");

  const agent::Completion completion = provider->complete(simple_conversation(), kNoTools, agent::CompleteOptions{});
  CHECK(completion.text == "plain answer");
  REQUIRE(completion.calls.size() == 1);
  CHECK(completion.calls[0].id == "call_x");
  CHECK(completion.calls[0].name == "get_timeline");
  CHECK(completion.calls[0].arguments["detail"] == true);
  CHECK(completion.input_tokens == 3);
  CHECK(completion.output_tokens == 4);
}

TEST_CASE("codex provider: an empty access token is rejected") {
  agent::ProviderConfig cfg;
  cfg.kind = "codex";
  cfg.api_key = "";
  CHECK_THROWS_AS(agent::OpenAiResponses(cfg), std::invalid_argument);
}
