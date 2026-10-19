// Tests for the HTTP/SSE layer and provider error mapping.
//
// Everything here is offline: the transport tests talk only to et::test::MockServer, and the parser tests never
// touch the network. The suite is deliberately fast (well under ten seconds) so it can run on every commit.
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "agent/http.h"
#include "agent/openai_chat.h"
#include "agent/provider.h"
#include "mock_http.h"

using namespace et;
using Catch::Matchers::ContainsSubstring;
using et::agent::json;

namespace {

constexpr const char* kSse = "text/event-stream";

agent::ProviderConfig provider_config(const std::string& base_url) {
  agent::ProviderConfig cfg;
  cfg.kind = "openai";
  cfg.model = "test-model";
  cfg.base_url = base_url;
  cfg.api_key = "test-key";
  return cfg;
}

std::vector<agent::Message> one_user_message(const std::string& text) {
  std::vector<agent::Message> convo;
  agent::Message sys;
  sys.role = agent::Message::Role::System;
  sys.text = "You are an editor.";
  convo.push_back(std::move(sys));
  agent::Message user;
  user.role = agent::Message::Role::User;
  user.text = text;
  convo.push_back(std::move(user));
  return convo;
}

// One OpenAI-style streamed chunk carrying assistant text.
json content_chunk(const std::string& text) {
  return {{"choices",
           json::array({{{"index", 0}, {"delta", {{"content", text}}}, {"finish_reason", nullptr}}})}};
}

// One OpenAI-style streamed chunk carrying (part of) a tool call.
json call_chunk(const std::string& id, const std::string& name, const std::string& args, int index = 0) {
  json function = {{"arguments", args}};
  if (!name.empty()) function["name"] = name;
  json call = {{"index", index}, {"function", std::move(function)}};
  if (!id.empty()) call["id"] = id;
  return {{"choices",
           json::array({{{"index", 0}, {"delta", {{"tool_calls", json::array({std::move(call)})}}}, {"finish_reason", nullptr}}})}};
}

// A complete streamed answer: one text chunk, a finish chunk and [DONE].
std::string sse_text_body(const std::string& text) {
  json fin = {{"choices", json::array({{{"index", 0}, {"delta", json::object()}, {"finish_reason", "stop"}}})}};
  return "data: " + content_chunk(text).dump() + "\n\ndata: " + fin.dump() + "\n\ndata: [DONE]\n\n";
}

// ---------------------------------------------------------------------------------------------------------------
// A deterministic pseudo-random corpus for ChatStreamParser. Fixed seed: a failure always reproduces.
// ---------------------------------------------------------------------------------------------------------------
class Rng {
 public:
  explicit Rng(unsigned seed) : gen_(seed) {}
  int below(int n) { return n <= 0 ? 0 : static_cast<int>(gen_() % static_cast<unsigned>(n)); }
  bool chance(int percent) { return below(100) < percent; }

  // Short gibberish drawn from an alphabet that cannot spell "index" or "usage", so a random line can never
  // hit a numeric field with a string (which would be a json type_error, not something the parser promises to
  // survive). Everything else -- braces, colons, quotes, CRs, stray "data" prefixes -- is fair game.
  std::string junk(int max_len) {
    static constexpr char kChars[] = "{}[]\":,0123456789abcdefg tfnl\r-_./";
    const int n = 1 + below(max_len);
    std::string s;
    s.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) s.push_back(kChars[below(static_cast<int>(sizeof kChars) - 1)]);
    return s;
  }

 private:
  std::mt19937 gen_;
};

// Tool-call argument fragments. Some are valid JSON, some are truncated, some are the wrong JSON type.
std::string random_arguments(Rng& rng) {
  switch (rng.below(9)) {
    case 0:
      return "";
    case 1:
      return "{\"time\": ";
    case 2:
      return "1.5";
    case 3:
      return "}";
    case 4:
      return "{\"time\":1.5}";
    case 5:
      return "[1,2,3]";
    case 6:
      return "null";
    case 7:
      return "{oops";
    default:
      return "{\"n\": " + std::to_string(rng.below(10)) + "}";
  }
}

// A plausible-but-broken tool_calls entry: missing id/name/index, huge indexes, wrong types, partial arguments.
json random_tool_call(Rng& rng) {
  json call = json::object();
  switch (rng.below(6)) {
    case 0:
      break;  // no index at all: the parser defaults it to 0
    case 1:
      call["index"] = 1000000;  // far past the 64-call cap: must be ignored, not allocated
      break;
    case 2:
      call["index"] = 64 + rng.below(500);
      break;
    default:
      call["index"] = rng.below(4);
      break;
  }
  if (rng.chance(70)) {
    static const char* kIds[] = {"call_a", "", "call_b"};
    call["id"] = kIds[rng.below(3)];
  }
  switch (rng.below(8)) {
    case 0:
      call["function"] = "not an object";
      break;
    case 1:
      call["function"] = json::array({"nope"});
      break;
    default: {
      json function = json::object();
      if (rng.chance(75)) {
        static const char* kNames[] = {"", "preview_frame", "get_", "timeline", "set_subtitles"};
        function["name"] = kNames[rng.below(5)];
      }
      if (rng.chance(80)) function["arguments"] = random_arguments(rng);
      if (rng.chance(5)) function["arguments"] = 42;  // wrong type: ignored
      call["function"] = std::move(function);
      break;
    }
  }
  return call;
}

json random_tool_call_entry(Rng& rng) {
  // A tool_calls entry is an object on the wire. Wrong-typed *fields* (function as a string, name as a number,
  // arguments as an array fragment) are exercised below; a wrong-typed entry (null, a bare string) is not,
  // because nlohmann's value("index", ...) throws on a non-object before the parser can inspect it.
  return random_tool_call(rng);
}

// A chunk whose delta mixes (duplicate, empty, partial) content with broken tool calls, and sometimes usage.
json random_delta_chunk(Rng& rng) {
  json delta = json::object();
  if (rng.chance(55)) delta["content"] = rng.chance(20) ? "" : "piece";
  if (rng.chance(45)) delta["role"] = "assistant";
  if (rng.chance(45)) {
    json calls = json::array();
    const int n = 1 + rng.below(3);
    for (int i = 0; i < n; ++i) calls.push_back(random_tool_call_entry(rng));
    delta["tool_calls"] = std::move(calls);
  }
  json chunk = {{"choices",
                 json::array({{{"index", 0},
                               {"delta", std::move(delta)},
                               {"finish_reason", rng.chance(10) ? json("stop") : json(nullptr)}}})}};
  if (rng.chance(10)) chunk["usage"] = {{"prompt_tokens", 7}, {"completion_tokens", 3}};
  return chunk;
}

// Thousands of lines: non-SSE noise, invalid/truncated JSON, broken tool_calls, duplicate content deltas,
// and several spellings of [DONE] injected in the middle with more lines after it.
std::vector<std::string> fuzz_corpus() {
  Rng rng(0xC0FFEEu);
  std::vector<std::string> lines;

  // Deterministic cases at indexes the fuzz never targets (the fuzz only touches 0..3, or past the 64 cap),
  // so both finish() branches -- parsed object and __invalid_arguments -- are guaranteed to run.
  lines.push_back("data: " + call_chunk("call_good", "get_timeline", "{}", 60).dump());
  lines.push_back("data: " + call_chunk("", "preview_frame", "{\"time\": ", 61).dump());
  lines.push_back("data: " + call_chunk("", "render_export", "{\"output_path\": \"x.mp4\"}", 62).dump());

  for (int i = 0; i < 4000; ++i) {
    switch (i % 5) {
      case 0:
        // Not SSE at all: HTTP-ish headers, comments, blank lines, an event: line, raw gibberish.
        switch (rng.below(6)) {
          case 0:
            lines.push_back("");
            break;
          case 1:
            lines.push_back(": keep-alive comment");
            break;
          case 2:
            lines.push_back("event: message");
            break;
          case 3:
            lines.push_back("Content-Type: text/event-stream");
            break;
          case 4:
            lines.push_back("data");
            break;
          default:
            lines.push_back(rng.junk(30));
            break;
        }
        break;
      case 1: {
        // data: with invalid or truncated JSON.
        std::string payload;
        switch (rng.below(4)) {
          case 0:
            payload = rng.junk(24);
            break;
          case 1: {
            const std::string good = content_chunk("hello").dump();
            payload = good.substr(0, static_cast<std::size_t>(rng.below(static_cast<int>(good.size()) + 1)));
            break;
          }
          case 2:
            payload = "{\"choices\":[";
            break;
          default:
            payload = "{\"choices\": [{\"delta\": {\"content\": \"unterminated";
            break;
        }
        lines.push_back(rng.chance(80) ? "data: " + payload : "data:" + payload);
        break;
      }
      case 2:
        lines.push_back("data: " + random_delta_chunk(rng).dump());
        break;
      case 3:
        // Duplicate and empty content deltas, exactly as a chatty server sends them.
        lines.push_back("data: " + content_chunk(rng.chance(30) ? "" : "tok").dump());
        break;
      default:
        lines.push_back(rng.junk(30));
        break;
    }
  }

  // [DONE] in the middle, in each spelling seen in the wild, with more traffic afterwards.
  const auto mid = static_cast<std::vector<std::string>::difference_type>(lines.size() / 2);
  lines.insert(lines.begin() + mid, {"data: [DONE]", "data:[DONE]", "data: [DONE] "});
  for (int i = 0; i < 500; ++i)
    lines.push_back(rng.chance(30) ? "data: " + random_delta_chunk(rng).dump()
                                   : "data: " + content_chunk("late").dump());
  return lines;
}

// The guarantee ChatStreamParser::finish documents: a produced call has a name, and either object arguments or
// the raw text under __invalid_arguments.
void check_tool_call_guarantee(const agent::ToolCall& call) {
  CHECK_FALSE(call.name.empty());
  CHECK_FALSE(call.id.empty());
  CHECK(call.arguments.is_object());
  if (call.arguments.contains("__invalid_arguments")) CHECK(call.arguments["__invalid_arguments"].is_string());
}

// A socket that accepts one connection and then never answers, keeping curl blocked mid-request. That lets the
// cancellation test abort a *running* request deterministically instead of racing curl's startup.
class HangingServer {
 public:
  HangingServer() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) throw std::runtime_error("hanging server: socket failed");
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
      throw std::runtime_error("hanging server: bind failed");
    socklen_t len = sizeof addr;
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
      throw std::runtime_error("hanging server: getsockname failed");
    port_ = ntohs(addr.sin_port);
    if (::listen(fd_, 4) != 0) throw std::runtime_error("hanging server: listen failed");
    thread_ = std::thread([this] {
      while (!stop_) {
        pollfd p{fd_, POLLIN, 0};
        if (::poll(&p, 1, 20) <= 0) continue;
        const int conn = ::accept(fd_, nullptr, nullptr);
        if (conn < 0) continue;
        conn_ = conn;  // leave it open: the client sees a request that never gets a response
        accepted_ = true;
        return;
      }
    });
  }
  ~HangingServer() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (conn_ >= 0) ::close(conn_);
    if (fd_ >= 0) ::close(fd_);
  }
  HangingServer(const HangingServer&) = delete;
  HangingServer& operator=(const HangingServer&) = delete;

  std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
  bool accepted() const { return accepted_; }

 private:
  int fd_ = -1, conn_ = -1, port_ = 0;
  std::atomic<bool> accepted_{false}, stop_{false};
  std::thread thread_;
};

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// 1. ChatStreamParser fuzzing (no network).
// ---------------------------------------------------------------------------------------------------------------

TEST_CASE("http: ChatStreamParser survives a fuzz corpus and still guarantees well-formed tool calls") {
  std::string streamed;
  agent::ChatStreamParser parser([&](const std::string& delta) { streamed += delta; });

  const std::vector<std::string> lines = fuzz_corpus();
  REQUIRE(lines.size() > 4000);
  for (const auto& line : lines) parser.feed_line(line);

  // finish() must not crash and must always produce a usable Completion.
  const agent::Completion c = parser.finish();
  CHECK(parser.done());  // the corpus contains [DONE]
  CHECK(c.text == streamed);
  CHECK(c.calls.size() <= 63u);  // indexes >= 64 are dropped instead of growing the vector
  CHECK((c.stop_reason.empty() || c.stop_reason == "stop"));
  CHECK((c.input_tokens == 0 || c.input_tokens == 7));  // usage chunks only ever carried 7/3

  bool saw_valid = false, saw_invalid = false;
  for (const auto& call : c.calls) {
    check_tool_call_guarantee(call);
    if (call.name == "get_timeline" && call.arguments.empty()) saw_valid = true;
    if (call.arguments.contains("__invalid_arguments")) saw_invalid = true;
  }
  // Both finish() branches were exercised by the deterministic seeds.
  CHECK(saw_valid);
  CHECK(saw_invalid);

  // An id-less call gets a stable synthetic id rather than an empty one.
  bool saw_synthetic_id = false;
  for (const auto& call : c.calls)
    if (call.id.rfind("call_", 0) == 0) saw_synthetic_id = true;
  CHECK(saw_synthetic_id);
}

TEST_CASE("http: ChatStreamParser reassembles a tool call split across data lines") {
  std::string streamed;
  agent::ChatStreamParser parser([&](const std::string& delta) { streamed += delta; });

  // The arguments JSON arrives in two pieces: `{"time":` and ` 1.5}`.
  parser.feed_line("data: " + call_chunk("call_a", "preview_frame", "{\"time\":", 0).dump());
  parser.feed_line("data: " + call_chunk("", "", " 1.5}", 0).dump());
  parser.feed_line("data: [DONE]");

  const agent::Completion c = parser.finish();
  REQUIRE(c.calls.size() == 1);
  CHECK(c.calls[0].id == "call_a");
  CHECK(c.calls[0].name == "preview_frame");
  CHECK(c.calls[0].arguments == json{{"time", 1.5}});
  CHECK_FALSE(c.calls[0].arguments.contains("__invalid_arguments"));
  CHECK(streamed.empty());  // tool-call chunks are not assistant text
  CHECK(parser.done());
}

// ---------------------------------------------------------------------------------------------------------------
// 2. http_post against MockServer.
// ---------------------------------------------------------------------------------------------------------------

TEST_CASE("http: a 200 response keeps the body and streams its lines") {
  test::MockServer server;
  server.enqueue(R"({"ok":true,"n":1})");

  agent::HttpRequest req;
  req.url = server.url("/v1/thing");
  req.headers = {"Content-Type: application/json", "Accept: text/event-stream"};
  req.body = R"({"hello":"world"})";
  req.timeout_sec = 10;

  std::vector<std::string> lines;
  const agent::HttpResult r = agent::http_post(req, [&](const std::string& line) { lines.push_back(line); });

  CHECK(r.status == 200);
  CHECK(r.error.empty());
  CHECK(r.ok());
  CHECK_THAT(r.body, ContainsSubstring(R"("ok":true)"));
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == R"({"ok":true,"n":1})");

  REQUIRE(server.request_count() == 1);
  CHECK(server.request(0).method == "POST");
  CHECK(server.request(0).path == "/v1/thing");
  CHECK(server.request(0).body == R"({"hello":"world"})");
  CHECK(server.request(0).header("content-type") == "application/json");
  CHECK(server.request(0).header("accept") == "text/event-stream");
}

TEST_CASE("http: a 500 response keeps the body but is not ok()") {
  test::MockServer server;
  server.enqueue(R"({"error":{"message":"boom"}})", 500);

  agent::HttpRequest req;
  req.url = server.url("/v1/thing");
  req.body = "{}";
  req.timeout_sec = 10;

  const agent::HttpResult r = agent::http_post(req);
  CHECK_FALSE(r.ok());
  CHECK(r.status == 500);
  CHECK(r.error.empty());  // transport was fine; the error document is the body
  CHECK_THAT(r.body, ContainsSubstring("boom"));
  CHECK(server.request_count() == 1);
}

TEST_CASE("http: a closed port is a fast transport error, not a hang") {
  // 127.0.0.1:1 is never listening. The short timeout is belt-and-braces: a hang would still fail the suite.
  agent::HttpRequest req;
  req.url = "http://127.0.0.1:1/v1/chat/completions";
  req.body = "{}";
  req.timeout_sec = 3;

  const auto start = std::chrono::steady_clock::now();
  const agent::HttpResult r = agent::http_post(req);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

  CHECK(r.status == 0);
  CHECK_FALSE(r.error.empty());
  CHECK_FALSE(r.ok());
  CHECK(ms < 5000);
}

TEST_CASE("http: cancellation aborts a running request quickly and reports it") {
  HangingServer server;  // connects, never answers: curl stays mid-request until cancelled

  agent::HttpRequest req;
  req.url = server.url("/chat/completions");
  req.body = "{}";
  req.timeout_sec = 30;

  std::atomic<bool> cancelled{false};
  agent::HttpResult result;
  const auto start = std::chrono::steady_clock::now();
  std::thread worker([&] { result = agent::http_post(req, {}, [&] { return cancelled.load(); }); });

  // Wait until curl has connected, so this is a real mid-request cancellation.
  for (int i = 0; i < 500 && !server.accepted(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  CHECK(server.accepted());
  cancelled = true;
  worker.join();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

  CHECK(result.status == 0);
  CHECK_FALSE(result.ok());
  CHECK_THAT(result.error, ContainsSubstring("cancelled"));
  CHECK(ms < 5000);
}

// ---------------------------------------------------------------------------------------------------------------
// 3. Provider error mapping through OpenAiChat::complete.
// ---------------------------------------------------------------------------------------------------------------

TEST_CASE("http: a 401 becomes a ProviderError carrying the API message") {
  test::MockServer server;
  server.enqueue(R"({"error":{"message":"invalid api key"}})", 401);

  agent::OpenAiChat provider(provider_config(server.base_url()));
  try {
    provider.complete(one_user_message("hello"), {}, {});
    FAIL("expected a ProviderError for HTTP 401");
  } catch (const agent::ProviderError& e) {
    CHECK(e.status == 401);
    CHECK_THAT(e.what(), ContainsSubstring("401"));
    CHECK_THAT(e.what(), ContainsSubstring("invalid api key"));
  }
  CHECK(server.request_count() == 1);  // 401 is not transient: no retry
  CHECK(server.request(0).path == "/chat/completions");
}

TEST_CASE("http: a 500 becomes a ProviderError with status 500") {
  test::MockServer server;
  server.enqueue(R"({"error":{"message":"model exploded"}})", 500);

  agent::OpenAiChat provider(provider_config(server.base_url()));
  try {
    provider.complete(one_user_message("hello"), {}, {});
    FAIL("expected a ProviderError for HTTP 500");
  } catch (const agent::ProviderError& e) {
    CHECK(e.status == 500);
    CHECK_THAT(e.what(), ContainsSubstring("500"));
    CHECK_THAT(e.what(), ContainsSubstring("model exploded"));
  }
  CHECK(server.request_count() == 1);  // only 429/502/503/504 are retried, not 500
}

TEST_CASE("http: a 429 is retried and the next response succeeds") {
  test::MockServer server;
  server.enqueue(R"({"error":{"message":"slow down"}})", 429);
  server.enqueue(sse_text_body("after the retry"), 200, kSse);

  std::vector<std::string> notices;
  agent::CompleteOptions opt;
  opt.on_retry = [&](const std::string& notice) { notices.push_back(notice); };

  agent::OpenAiChat provider(provider_config(server.base_url()));
  const agent::Completion c = provider.complete(one_user_message("hello"), {}, opt);

  CHECK(c.text == "after the retry");
  CHECK(c.stop_reason == "stop");
  CHECK(c.calls.empty());
  REQUIRE(server.request_count() == 2);  // the same request was retried
  CHECK(server.request(0).path == "/chat/completions");
  CHECK(server.request(1).body == server.request(0).body);
  REQUIRE(notices.size() == 1);
  CHECK_THAT(notices[0], ContainsSubstring("429"));
}

TEST_CASE("http: malformed SSE with no content is a ProviderError, not a crash") {
  test::MockServer server;
  // No [DONE]: the stream just stops, having delivered nothing usable.
  server.enqueue(
      "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"\"}}]}\n\ndata: {\"choices\":\n\ndata: not json at "
      "all\n\ndata: [DONE-ish]\n\n",
      200, kSse);

  agent::OpenAiChat provider(provider_config(server.base_url()));
  try {
    provider.complete(one_user_message("hello"), {}, {});
    FAIL("expected a ProviderError for a contentless SSE stream");
  } catch (const agent::ProviderError& e) {
    CHECK(e.status == 200);
    CHECK_THAT(e.what(), ContainsSubstring("no content"));
  }
  CHECK(server.request_count() == 1);

  // A stream that does end with [DONE] but carries no content is an empty turn, not an exception: the server
  // explicitly said it was finished, so complete() returns instead of guessing.
  server.enqueue("data: {\"choices\":[{\"index\":0,\"delta\":{}}]}\n\ndata: [DONE]\n\n", 200, kSse);
  const agent::Completion empty = provider.complete(one_user_message("hello"), {}, {});
  CHECK(empty.text.empty());
  CHECK(empty.calls.empty());
  CHECK(server.request_count() == 2);
}

// ---------------------------------------------------------------------------------------------------------------
// 4. error_message_from_body.
// ---------------------------------------------------------------------------------------------------------------

TEST_CASE("http: error_message_from_body prefers the API message and falls back to a trimmed excerpt") {
  using agent::error_message_from_body;

  // The three shapes providers actually send.
  CHECK(error_message_from_body(R"({"error":{"message":"rate limited"}})") == "rate limited");
  CHECK(error_message_from_body(R"({"error":"plain error string"})") == "plain error string");
  CHECK(error_message_from_body(R"({"message":"top-level message"})") == "top-level message");

  // Wrong types fall through to the excerpt instead of throwing.
  CHECK(error_message_from_body(R"({"error":42})") == R"({"error":42})");
  CHECK(error_message_from_body(R"({"error":{"message":42}})") == R"({"error":{"message":42}})");
  CHECK(error_message_from_body("[1,2,3]") == "[1,2,3]");

  // HTML and plain text: trailing whitespace is trimmed, leading content is preserved.
  CHECK(error_message_from_body("<html>\n<body>502 Bad Gateway</body>\n</html>\n") ==
        "<html>\n<body>502 Bad Gateway</body>\n</html>");
  CHECK(error_message_from_body("  server exploded  \n\n") == "  server exploded");

  // At most a 300-byte excerpt, so a huge HTML page cannot flood a ProviderError.
  CHECK(error_message_from_body(std::string(400, 'x')) == std::string(300, 'x'));
  CHECK(error_message_from_body("") == "");
}

// Malformed entries used to escape as raw nlohmann exceptions (type_error.306) instead of being ignored.
TEST_CASE("http: malformed tool_call entries and usage fields are ignored, not fatal") {
  agent::ChatStreamParser parser(nullptr);
  parser.feed_line(R"(data: {"choices":[{"delta":{"tool_calls":["garbage",null,42,{}]}}]})");
  parser.feed_line(R"(data: {"usage":{"prompt_tokens":"lots","completion_tokens":null}})");
  parser.feed_line(
      R"(data: {"choices":[{"delta":{"tool_calls":[{"index":"zero","function":"nope"}]}}]})");
  parser.feed_line(
      R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"c1","function":{"name":"preview_frame","arguments":"{\"time\":1}"}}]}}]})");
  parser.feed_line("data: [DONE]");

  const agent::Completion c = parser.finish();
  CHECK(parser.done());
  CHECK(c.input_tokens == 0);   // the string usage value is ignored
  CHECK(c.output_tokens == 0);  // null is ignored too
  REQUIRE(c.calls.size() == 1);  // only the well-formed call survives
  CHECK(c.calls[0].name == "preview_frame");
  CHECK(c.calls[0].arguments["time"] == 1);
  CHECK(c.calls[0].id == "c1");
}
