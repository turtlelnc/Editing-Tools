#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "agent/anthropic.h"
#include "mock_http.h"

using Catch::Matchers::ContainsSubstring;
using namespace et;
using agent::json;

namespace {

constexpr const char* kSse = "text/event-stream";

agent::ProviderConfig config_for(const std::string& base_url) {
  agent::ProviderConfig cfg;
  cfg.kind = "anthropic";
  cfg.model = "claude-test";
  cfg.api_key = "test-key";
  cfg.base_url = base_url;
  return cfg;
}

agent::Message message(agent::Message::Role role, const std::string& text) {
  agent::Message m;
  m.role = role;
  m.text = text;
  return m;
}

agent::Message tool_message(const std::string& call_id, const std::string& text) {
  agent::Message m = message(agent::Message::Role::Tool, text);
  m.call_id = call_id;
  m.tool_name = "preview_frame";
  return m;
}

agent::ToolSpec frame_tool() {
  agent::ToolSpec t;
  t.name = "preview_frame";
  t.description = "Show a frame";
  t.schema = {{"type", "object"},
              {"properties", {{"time", {{"type", "number"}}}}},
              {"required", json::array({"time"})}};
  return t;
}

// --- SSE scripting (Anthropic Messages events) -------------------------------------------------

std::string event(const std::string& type, const json& data) {
  return "event: " + type + "\ndata: " + data.dump() + "\n\n";
}

std::string message_start(long input_tokens) {
  return event("message_start", {{"type", "message_start"},
                                 {"message", {{"id", "msg_1"},
                                              {"type", "message"},
                                              {"role", "assistant"},
                                              {"content", json::array()},
                                              {"model", "claude-test"},
                                              {"stop_reason", nullptr},
                                              {"usage", {{"input_tokens", input_tokens}, {"output_tokens", 1}}}}}});
}

std::string text_block(int index, const std::vector<std::string>& deltas) {
  std::string s = event("content_block_start", {{"type", "content_block_start"},
                                                {"index", index},
                                                {"content_block", {{"type", "text"}, {"text", ""}}}});
  for (const auto& d : deltas)
    s += event("content_block_delta", {{"type", "content_block_delta"},
                                       {"index", index},
                                       {"delta", {{"type", "text_delta"}, {"text", d}}}});
  s += event("content_block_stop", {{"type", "content_block_stop"}, {"index", index}});
  return s;
}

std::string tool_block(int index, const std::string& id, const std::string& name,
                       const std::vector<std::string>& arg_chunks) {
  std::string s = event("content_block_start",
                        {{"type", "content_block_start"},
                         {"index", index},
                         {"content_block",
                          {{"type", "tool_use"}, {"id", id}, {"name", name}, {"input", json::object()}}}});
  for (const auto& chunk : arg_chunks)
    s += event("content_block_delta", {{"type", "content_block_delta"},
                                       {"index", index},
                                       {"delta", {{"type", "input_json_delta"}, {"partial_json", chunk}}}});
  s += event("content_block_stop", {{"type", "content_block_stop"}, {"index", index}});
  return s;
}

std::string message_end(const std::string& stop_reason, long output_tokens) {
  return event("message_delta", {{"type", "message_delta"},
                                 {"delta", {{"stop_reason", stop_reason}, {"stop_sequence", nullptr}}},
                                 {"usage", {{"output_tokens", output_tokens}}}}) +
         event("message_stop", {{"type", "message_stop"}});
}

std::string streamed_answer() {
  return message_start(42) + text_block(0, {"Let me look. ", "Now."}) +
         tool_block(1, "toolu_7", "preview_frame", {R"({"time":)", R"(2.5})"}) + message_end("tool_use", 17);
}

}  // namespace

TEST_CASE("agent: anthropic request hoists system, translates images and merges tool results") {
  std::vector<agent::Message> convo;
  convo.push_back(message(agent::Message::Role::System, "You are an editor."));
  convo.push_back(message(agent::Message::Role::System, "Be brief."));

  agent::Message user_m = message(agent::Message::Role::User, "look at this");
  user_m.images.push_back({"image/jpeg", "QUJD"});
  convo.push_back(user_m);

  agent::Message assistant = message(agent::Message::Role::Assistant, "opening");
  assistant.calls.push_back({"toolu_1", "preview_frame", {{"time", 1.0}}});
  assistant.calls.push_back({"toolu_2", "get_timeline", json::object()});
  convo.push_back(assistant);

  agent::Message first = tool_message("toolu_1", "first result");
  first.images.push_back({"image/png", "aW1nMg=="});
  convo.push_back(first);
  agent::Message second = tool_message("toolu_2", "second result");
  second.is_error = true;
  convo.push_back(second);

  std::vector<agent::ToolSpec> tools = {frame_tool()};
  agent::Anthropic provider(config_for("http://127.0.0.1:1"));
  const json body = provider.build_request(convo, tools);

  CHECK(body["model"] == "claude-test");
  CHECK(body["max_tokens"] == 8192);
  CHECK(body["stream"] == true);
  CHECK(body["system"] == "You are an editor.\n\nBe brief.");

  REQUIRE(body["messages"].size() == 3);
  for (const auto& m : body["messages"]) CHECK(m["role"] != "system");

  // The user's picture becomes an Anthropic image block in the user turn.
  const json& u = body["messages"][0];
  REQUIRE(u["role"] == "user");
  REQUIRE(u["content"].is_array());
  REQUIRE(u["content"].size() == 2);
  CHECK(u["content"][0]["type"] == "text");
  CHECK(u["content"][0]["text"] == "look at this");
  CHECK(u["content"][1]["type"] == "image");
  CHECK(u["content"][1]["source"]["type"] == "base64");
  CHECK(u["content"][1]["source"]["media_type"] == "image/jpeg");
  CHECK(u["content"][1]["source"]["data"] == "QUJD");

  // The assistant turn keeps its text and turns every call into a tool_use block.
  const json& a = body["messages"][1];
  REQUIRE(a["role"] == "assistant");
  REQUIRE(a["content"].size() == 3);
  CHECK(a["content"][0]["type"] == "text");
  CHECK(a["content"][0]["text"] == "opening");
  CHECK(a["content"][1]["type"] == "tool_use");
  CHECK(a["content"][1]["id"] == "toolu_1");
  CHECK(a["content"][1]["name"] == "preview_frame");
  CHECK(a["content"][1]["input"]["time"] == 1.0);
  CHECK(a["content"][2]["type"] == "tool_use");
  CHECK(a["content"][2]["id"] == "toolu_2");
  CHECK(a["content"][2]["name"] == "get_timeline");

  // Both tool results share one user message, and the tool's image rides inside its own tool_result.
  const json& t = body["messages"][2];
  REQUIRE(t["role"] == "user");
  REQUIRE(t["content"].size() == 2);
  CHECK(t["content"][0]["type"] == "tool_result");
  CHECK(t["content"][0]["tool_use_id"] == "toolu_1");
  CHECK(t["content"][0]["is_error"] == false);
  REQUIRE(t["content"][0]["content"].size() == 2);
  CHECK(t["content"][0]["content"][0]["type"] == "text");
  CHECK(t["content"][0]["content"][0]["text"] == "first result");
  CHECK(t["content"][0]["content"][1]["type"] == "image");
  CHECK(t["content"][0]["content"][1]["source"]["media_type"] == "image/png");
  CHECK(t["content"][0]["content"][1]["source"]["data"] == "aW1nMg==");
  CHECK(t["content"][1]["type"] == "tool_result");
  CHECK(t["content"][1]["tool_use_id"] == "toolu_2");
  CHECK(t["content"][1]["is_error"] == true);

  // Tools carry an input_schema, and nothing else is invented.
  REQUIRE(body["tools"].size() == 1);
  CHECK(body["tools"][0]["name"] == "preview_frame");
  CHECK(body["tools"][0]["description"] == "Show a frame");
  REQUIRE(body["tools"][0]["input_schema"]["type"] == "object");
  CHECK(body["tools"][0]["input_schema"]["properties"]["time"]["type"] == "number");
  CHECK(body["tools"][0]["input_schema"]["required"][0] == "time");
  CHECK_FALSE(body["tools"][0].contains("input"));

  // Without system text or tools the fields are omitted rather than sent empty.
  const json bare = provider.build_request({message(agent::Message::Role::User, "hi")}, {});
  CHECK_FALSE(bare.contains("system"));
  CHECK_FALSE(bare.contains("tools"));
  CHECK(bare["messages"][0]["content"][0]["text"] == "hi");
}

TEST_CASE("agent: anthropic streams text and tool calls with usage and the right headers") {
  et::test::MockServer server;
  // A trailing slash in the configured base URL must not double up in the request path.
  agent::Anthropic provider(config_for(server.base_url() + "/"));
  CHECK(provider.base_url() == server.base_url());

  std::vector<agent::Message> convo = {message(agent::Message::Role::System, "You are an editor."),
                                       message(agent::Message::Role::User, "show me a frame")};
  std::vector<agent::ToolSpec> tools = {frame_tool()};
  server.enqueue(streamed_answer(), 200, kSse);

  std::string streamed;
  agent::CompleteOptions opt;
  opt.on_text_delta = [&](const std::string& delta) { streamed += delta; };
  const agent::Completion c = provider.complete(convo, tools, opt);

  CHECK(c.text == "Let me look. Now.");
  CHECK(streamed == "Let me look. Now.");  // the caller saw the deltas as they arrived
  REQUIRE(c.calls.size() == 1);
  CHECK(c.calls[0].id == "toolu_7");
  CHECK(c.calls[0].name == "preview_frame");
  CHECK(c.calls[0].arguments["time"] == 2.5);  // stitched together from two input_json_delta chunks
  CHECK(c.stop_reason == "tool_use");
  CHECK(c.input_tokens == 42);
  CHECK(c.output_tokens == 17);

  REQUIRE(server.request_count() == 1);
  const et::test::MockServer::Request req = server.request(0);
  CHECK(req.method == "POST");
  CHECK(req.path == "/v1/messages");
  CHECK(req.header("x-api-key") == "test-key");
  CHECK(req.header("anthropic-version") == "2023-06-01");
  CHECK_THAT(req.header("content-type"), ContainsSubstring("application/json"));
  CHECK_THAT(req.header("accept"), ContainsSubstring("text/event-stream"));
  const json sent = json::parse(req.body);
  CHECK(sent["stream"] == true);
  CHECK(sent["system"] == "You are an editor.");
  CHECK(sent["messages"].size() == 1);
}

TEST_CASE("agent: anthropic keeps invalid tool arguments visible to the model") {
  et::test::MockServer server;
  agent::Anthropic provider(config_for(server.base_url()));
  server.enqueue(message_start(9) + tool_block(0, "toolu_bad", "preview_frame", {R"({"time":)"}) + message_end("tool_use", 4),
                 200, kSse);

  const agent::Completion c = provider.complete({message(agent::Message::Role::User, "show a frame")}, {frame_tool()},
                                                agent::CompleteOptions{});
  REQUIRE(c.calls.size() == 1);
  CHECK(c.calls[0].name == "preview_frame");
  CHECK(c.calls[0].id == "toolu_bad");
  REQUIRE(c.calls[0].arguments.contains("__invalid_arguments"));
  CHECK(c.calls[0].arguments["__invalid_arguments"] == R"({"time":)");
  CHECK(c.input_tokens == 9);
  CHECK(c.output_tokens == 4);
}

TEST_CASE("agent: anthropic names a tool call that arrived without an id") {
  et::test::MockServer server;
  agent::Anthropic provider(config_for(server.base_url()));
  server.enqueue(message_start(3) + text_block(0, {"on it"}) + tool_block(1, "", "get_timeline", {"{}"}) +
                     message_end("tool_use", 2),
                 200, kSse);

  const agent::Completion c = provider.complete({message(agent::Message::Role::User, "timeline")}, {},
                                                agent::CompleteOptions{});
  CHECK(c.text == "on it");
  REQUIRE(c.calls.size() == 1);
  CHECK(c.calls[0].id == "call_1");
  CHECK(c.calls[0].name == "get_timeline");
  CHECK(c.calls[0].arguments.is_object());
  CHECK(c.calls[0].arguments.empty());
}

TEST_CASE("agent: anthropic errors surface as ProviderError with the HTTP status") {
  et::test::MockServer server;
  agent::Anthropic provider(config_for(server.base_url()));
  const json body = {{"type", "error"},
                     {"error", {{"type", "authentication_error"}, {"message", "invalid x-api-key"}}}};
  server.enqueue(body.dump(), 401);

  try {
    provider.complete({message(agent::Message::Role::User, "hello")}, {}, agent::CompleteOptions{});
    FAIL("expected a ProviderError");
  } catch (const agent::ProviderError& e) {
    CHECK(e.status == 401);
    CHECK_THAT(e.what(), ContainsSubstring("401"));
    CHECK_THAT(e.what(), ContainsSubstring("invalid x-api-key"));
  }
  CHECK(server.request_count() == 1);  // a 401 is not retried
  CHECK(server.request(0).header("x-api-key") == "test-key");
}

TEST_CASE("agent: anthropic reports an error event from inside the stream") {
  et::test::MockServer server;
  agent::Anthropic provider(config_for(server.base_url()));
  server.enqueue(message_start(5) +
                     event("error", {{"type", "error"},
                                     {"error", {{"type", "overloaded_error"}, {"message", "Overloaded"}}}}),
                 200, kSse);

  try {
    provider.complete({message(agent::Message::Role::User, "hello")}, {}, agent::CompleteOptions{});
    FAIL("expected a ProviderError");
  } catch (const agent::ProviderError& e) {
    CHECK_THAT(e.what(), ContainsSubstring("Overloaded"));
  }
}

TEST_CASE("agent: anthropic salvages a non-streamed JSON answer") {
  et::test::MockServer server;
  agent::Anthropic provider(config_for(server.base_url()));
  const json doc = {
      {"id", "msg_1"},
      {"type", "message"},
      {"role", "assistant"},
      {"content", json::array({{{"type", "text"}, {"text", "Here is the plan."}},
                               {{"type", "tool_use"},
                                {"id", "toolu_9"},
                                {"name", "get_timeline"},
                                {"input", {{"compact", true}}}}})},
      {"stop_reason", "tool_use"},
      {"usage", {{"input_tokens", 11}, {"output_tokens", 22}}}};
  server.enqueue(doc.dump(), 200, "application/json");

  const agent::Completion c = provider.complete({message(agent::Message::Role::User, "plan it")}, {},
                                                agent::CompleteOptions{});
  CHECK(c.text == "Here is the plan.");
  REQUIRE(c.calls.size() == 1);
  CHECK(c.calls[0].id == "toolu_9");
  CHECK(c.calls[0].name == "get_timeline");
  CHECK(c.calls[0].arguments["compact"] == true);
  CHECK(c.stop_reason == "tool_use");
  CHECK(c.input_tokens == 11);
  CHECK(c.output_tokens == 22);
}

TEST_CASE("agent: anthropic applies its defaults and rejects a missing key") {
  agent::ProviderConfig cfg;
  cfg.api_key = "test-key";
  cfg.base_url = "https://api.example.test///";
  agent::Anthropic provider(cfg);
  CHECK(provider.model() == "claude-sonnet-5-5");
  CHECK(provider.base_url() == "https://api.example.test");
  CHECK(provider.build_request({}, {})["max_tokens"] == agent::Anthropic::kMaxTokens);

  agent::ProviderConfig keyless;
  keyless.model = "claude-test";
  CHECK_THROWS_AS(agent::Anthropic(keyless), std::invalid_argument);
}

TEST_CASE("agent: anthropic ignores malformed event fields instead of throwing") {
  agent::AnthropicStreamParser parser(nullptr);
  // Wrong types in the event envelope and in content blocks must not escape as nlohmann exceptions.
  parser.feed_line(R"(data: {"type":42})");
  parser.feed_line(R"(data: {"type":"content_block_start","index":0,"content_block":{"type":7}})");
  parser.feed_line(R"(data: {"type":"content_block_delta","index":0,"delta":{"type":[],"text":9}})");
  parser.feed_line(R"(data: {"type":"content_block_start","index":1,"content_block":{"type":"tool_use","name":9,"id":7}})");
  parser.feed_line(R"(data: {"type":"message_stop"})");
  const agent::Completion c = parser.finish();
  CHECK(parser.done());
  CHECK(c.text.empty());
  CHECK(c.calls.empty());
}
