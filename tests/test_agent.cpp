#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "agent/loop.h"
#include "agent/openai_chat.h"
#include "agent/policy.h"
#include "agent/prompt.h"
#include "agent/session.h"
#include "agent/toolbox.h"
#include "mock_http.h"
#include "process.h"
#include "tools/all.h"
#include "util/base64.h"

using namespace et;
using Catch::Matchers::ContainsSubstring;
using mcp::json;
namespace fs = std::filesystem;

namespace {

int next_suffix() {
  static std::atomic<int> n{0};
  return n++;
}

// A throw-away folder with a real 3-second clip, like the rest of the suite uses.
struct Dir {
  fs::path path;
  std::string video;
  Dir() {
    path = fs::temp_directory_path() / ("et_agent_" + std::to_string(::getpid()) + "_" + std::to_string(next_suffix()));
    fs::remove_all(path);
    fs::create_directories(path);
    video = (path / "clip.mp4").string();
    auto r = run({"ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i", "testsrc=size=160x120:rate=10:duration=3", "-f",
                  "lavfi", "-i", "sine=duration=3", "-shortest", "-pix_fmt", "yuv420p", video});
    REQUIRE(r.exit_code == 0);
  }
  ~Dir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  std::string operator()(const std::string& name) const { return (path / name).string(); }
};

constexpr const char* kSse = "text/event-stream";

std::string sse_text(const std::string& text) {
  json chunk = {{"choices", json::array({{{"index", 0}, {"delta", {{"content", text}}}, {"finish_reason", nullptr}}})}};
  json fin = {{"choices", json::array({{{"index", 0}, {"delta", json::object()}, {"finish_reason", "stop"}}})}};
  return "data: " + chunk.dump() + "\n\ndata: " + fin.dump() + "\n\ndata: [DONE]\n\n";
}

std::string sse_call(const std::string& id, const std::string& name, const json& args) {
  json call = {{"index", 0}, {"id", id}, {"type", "function"}, {"function", {{"name", name}, {"arguments", args.dump()}}}};
  json chunk = {{"choices", json::array({{{"index", 0}, {"delta", {{"tool_calls", json::array({call})}}}, {"finish_reason", nullptr}}})}};
  json fin = {{"choices", json::array({{{"index", 0}, {"delta", json::object()}, {"finish_reason", "tool_calls"}}})}};
  return "data: " + chunk.dump() + "\n\ndata: " + fin.dump() + "\n\ndata: [DONE]\n\n";
}

// A provider + toolbox wired to a mock server and a temporary workspace.
struct Harness {
  test::MockServer server;
  Dir& dir;
  std::shared_ptr<tools::Session> session = std::make_shared<tools::Session>();
  std::unique_ptr<agent::ToolBox> toolbox;
  std::unique_ptr<agent::OpenAiChat> provider;
  std::vector<agent::Message> convo;

  explicit Harness(Dir& d, bool overwrite = false) : dir(d) {
    agent::PolicyConfig pc;
    pc.roots = {dir.path};
    pc.overwrite = overwrite;
    toolbox = std::make_unique<agent::ToolBox>(session, agent::Policy(pc));
    agent::ProviderConfig cfg;
    cfg.kind = "openai";
    cfg.model = "test-model";
    cfg.base_url = server.base_url();
    cfg.api_key = "test-key";
    provider = std::make_unique<agent::OpenAiChat>(cfg);
    agent::Message sys;
    sys.role = agent::Message::Role::System;
    sys.text = "You are an editor.";
    convo.push_back(std::move(sys));
  }
  void user(const std::string& text) {
    agent::Message m;
    m.role = agent::Message::Role::User;
    m.text = text;
    convo.push_back(std::move(m));
  }
  void sse(std::string body) { server.enqueue(std::move(body), 200, kSse); }
};

}  // namespace

TEST_CASE("agent: a scripted model edits a real project through the tool loop") {
  Dir dir;
  Harness h(dir);
  const std::string project = dir("p.json");

  h.sse(sse_call("c1", "create_project",
                 {{"project_path", project}, {"width", 160}, {"height", 120}, {"fps", 10}}));
  h.sse(sse_call("c2", "add_clip", {{"path", dir.video}}));
  h.sse(sse_call("c3", "preview_frame", {{"time", 1.0}}));
  h.sse(sse_text("Done: one clip, previewed."));
  h.user("Put clip.mp4 in a project and show me a frame.");

  agent::AgentLoop loop(*h.provider, *h.toolbox);
  const auto result = loop.run(h.convo);

  CHECK(result.final_text == "Done: one clip, previewed.");
  CHECK(result.steps == 4);
  REQUIRE(h.server.request_count() == 4);

  // The project really exists on disk with the clip in it.
  std::ifstream f(project);
  REQUIRE(f);
  const json pj = json::parse(f);
  REQUIRE(pj["clips"].size() == 1);
  CHECK(pj["clips"][0]["out"] == 3.0);

  // Every request is a streamed chat completion carrying the tool schemas.
  for (size_t i = 0; i < 4; ++i) {
    CHECK(h.server.request(i).path == "/chat/completions");
    const json body = json::parse(h.server.request(i).body);
    CHECK(body["stream"] == true);
    CHECK(body["tools"].size() >= 20);
  }

  // Step 2 sees the create_project result as a tool message.
  {
    const json body = json::parse(h.server.request(1).body);
    const json& last = body["messages"].back();
    REQUIRE(last["role"] == "tool");
    CHECK(last["tool_call_id"] == "c1");
    CHECK_THAT(last["content"].get<std::string>(), ContainsSubstring("\"clips\""));
  }

  // Step 4 is where preview_frame's picture arrives: it must be attached as an image the model can see.
  {
    const json body = json::parse(h.server.request(3).body);
    const std::string dump = body.dump();
    CHECK_THAT(dump, ContainsSubstring("data:image/jpeg;base64,"));
    bool saw_assistant_calls = false;
    for (const auto& m : body["messages"])
      if (m.value("role", "") == "assistant" && m.contains("tool_calls")) saw_assistant_calls = true;
    CHECK(saw_assistant_calls);
  }

  // The conversation kept every step, including the tool-produced image.
  size_t tool_messages = 0, images = 0;
  for (const auto& m : h.convo) {
    if (m.role == agent::Message::Role::Tool) ++tool_messages;
    images += m.images.size();
  }
  CHECK(tool_messages == 3);
  CHECK(images >= 1);
}

TEST_CASE("agent: the policy keeps the agent inside its folders and refuses silent overwrites") {
  Dir dir;
  const fs::path outside = fs::temp_directory_path() / ("et_agent_outside_" + std::to_string(::getpid()));
  {
    std::ofstream f(outside);
    f << "not yours";
  }

  agent::PolicyConfig cfg;
  cfg.roots = {dir.path};
  agent::Policy policy(cfg);

  CHECK(policy.allowed(dir.path / "a.mp4"));
  CHECK_FALSE(policy.allowed(outside));
  CHECK_THROWS_AS(policy.check("add_clip", {{"path", outside.string()}}), std::invalid_argument);
  CHECK_THROWS_AS(policy.check("add_clip", {{"path", "../escape.mp4"}}), std::invalid_argument);

  // Relative names resolve inside the workspace.
  const json checked = policy.check("add_clip", {{"path", "a.mp4"}});
  CHECK(checked["path"].get<std::string>() == fs::weakly_canonical(dir.path / "a.mp4").string());

  // A finished file is never replaced without --overwrite.
  {
    std::ofstream f(dir("out.mp4"));
    f << "already here";
  }
  CHECK_THROWS_AS(policy.check("render_export", {{"output_path", "out.mp4"}}), std::invalid_argument);
  CHECK_NOTHROW(policy.check("render_export", {{"output_path", "new.mp4"}}));

  agent::PolicyConfig cfg2 = cfg;
  cfg2.overwrite = true;
  agent::Policy permissive(cfg2);
  CHECK_NOTHROW(permissive.check("render_export", {{"output_path", "out.mp4"}}));

  // Non-path arguments are left alone.
  CHECK(policy.check("preview_frame", {{"time", 1.5}, {"max_width", 320}})["time"] == 1.5);
  std::error_code ec;
  fs::remove(outside, ec);
}

TEST_CASE("agent: tool results are compacted so long sessions stay readable") {
  const json project = json::parse(R"json({
    "name": "demo", "width": 640, "height": 360, "fps": 25.0, "duration": 4.0,
    "assets": [{"id": "a1", "path": "/m/a.mp4", "duration": 4.0, "has_audio": true}],
    "clips": [{
      "id": "c1", "asset": "a1", "in": 0.0, "out": 4.0, "start": 0.0,
      "audio": {"volume_db": 0.0, "fade_in": 0.0, "fade_out": 0.0, "denoise": false},
      "color": {"brightness": 0.0, "contrast": 1.0, "saturation": 1.0, "gamma": 1.0, "warmth": 0.0,
                "tint": 0.0, "curve_preset": "", "curve_points": [], "lut_path": "", "lut_intensity": 1.0}
    }],
    "subtitles": []
  })json");

  const json compact = agent::compact_project(project);
  REQUIRE(compact["clips"].size() == 1);
  CHECK_FALSE(compact["clips"][0].contains("audio"));  // all-default properties are dropped
  CHECK_FALSE(compact["clips"][0].contains("color"));
  CHECK(compact["assets"][0]["audio"] == true);
  CHECK_FALSE(compact.contains("music"));

  // A non-neutral grade and audio settings survive.
  json edited = project;
  edited["clips"][0]["color"]["warmth"] = 0.3;
  edited["clips"][0]["audio"]["denoise"] = true;
  const json compact2 = agent::compact_project(edited);
  CHECK(compact2["clips"][0]["color"]["warmth"] == 0.3);
  CHECK(compact2["clips"][0]["audio"]["denoise"] == true);
  CHECK_FALSE(compact2["clips"][0]["audio"].contains("fade_in"));
}

TEST_CASE("agent: sessions are saved, listed and resumed with their images") {
  const fs::path root = fs::temp_directory_path() / ("et_agent_sessions_" + std::to_string(::getpid()));
  fs::remove_all(root);
  std::string id;
  {
    auto store = agent::SessionStore::create(root, "warm autumn cut");
    id = store.id();
    agent::Message u;
    u.role = agent::Message::Role::User;
    u.text = "hello, look at this frame";
    u.images.push_back({"image/jpeg", util::base64("fake-jpeg-bytes")});
    store.append(u);
    agent::Message a;
    a.role = agent::Message::Role::Assistant;
    a.text = "got it";
    store.append(a);
    store.set_config("workspace", "/tmp/workspace");
  }

  const auto list = agent::SessionStore::list(root);
  REQUIRE(list.size() == 1);
  CHECK(list[0].id == id);
  CHECK(list[0].title == "warm autumn cut");
  CHECK(list[0].messages == 2);

  const auto dir = agent::SessionStore::locate(root, "last");
  REQUIRE(dir.has_value());
  CHECK(dir->filename().string() == id);
  auto reopened = agent::SessionStore::open_dir(*dir);
  REQUIRE(reopened.has_value());
  REQUIRE(reopened->messages().size() == 2);
  CHECK(reopened->messages()[0].text == "hello, look at this frame");
  REQUIRE(reopened->messages()[0].images.size() == 1);
  CHECK(util::unbase64(reopened->messages()[0].images[0].data) == "fake-jpeg-bytes");
  CHECK(reopened->messages()[1].text == "got it");
  CHECK(reopened->config("workspace") == "/tmp/workspace");
  CHECK(reopened->config("nope").empty());  // missing keys read as empty

  std::error_code ec;
  fs::remove_all(root, ec);
}

TEST_CASE("agent: long conversations are summarized instead of flooding the context") {
  Dir dir;
  Harness h(dir);
  h.convo.clear();
  agent::Message sys;
  sys.role = agent::Message::Role::System;
  sys.text = "You are an editor.";
  h.convo.push_back(sys);
  const std::string filler(400, 'x');
  for (int i = 0; i < 3; ++i) {
    agent::Message u;
    u.role = agent::Message::Role::User;
    u.text = "task " + std::to_string(i) + " " + filler;
    h.convo.push_back(u);
    agent::Message a;
    a.role = agent::Message::Role::Assistant;
    a.text = "did it " + filler;
    h.convo.push_back(a);
  }
  h.user("now the new task");

  agent::LoopConfig cfg;
  cfg.compact_after_chars = 300;  // tiny, so the summary happens on the first step
  cfg.keep_recent_messages = 2;
  h.sse(sse_text("SUMMARY OF OLD TURNS"));
  h.sse(sse_text("the new answer"));

  agent::AgentLoop loop(*h.provider, *h.toolbox, cfg);
  const auto result = loop.run(h.convo);

  CHECK(result.final_text == "the new answer");
  REQUIRE(h.server.request_count() == 2);
  const json summarize = json::parse(h.server.request(0).body);
  CHECK_FALSE(summarize.contains("tools"));  // the summary call cannot touch the edit
  CHECK_THAT(h.server.request(1).body, ContainsSubstring("Summary of the earlier conversation"));
  CHECK_THAT(h.server.request(1).body, ContainsSubstring("SUMMARY OF OLD TURNS"));
  REQUIRE(h.convo.size() >= 2);
  CHECK_THAT(h.convo[1].text, ContainsSubstring("SUMMARY OF OLD TURNS"));
}

TEST_CASE("agent: an empty or cancelled step limit ends with words, not a dangling tool call") {
  Dir dir;
  Harness h(dir);
  h.sse(sse_call("c1", "get_timeline", json::object()));
  h.sse(sse_call("c2", "get_timeline", json::object()));
  h.sse(sse_text("I stopped early; the timeline is unchanged."));
  h.user("look at the timeline");

  agent::LoopConfig cfg;
  cfg.max_steps = 2;
  agent::AgentLoop loop(*h.provider, *h.toolbox, cfg);
  const auto result = loop.run(h.convo);

  CHECK(result.hit_max_steps);
  CHECK(result.final_text == "I stopped early; the timeline is unchanged.");
  CHECK(result.steps == 3);  // two tool steps plus the wrap-up
  REQUIRE(h.convo.back().role == agent::Message::Role::Assistant);
  CHECK(h.convo.back().calls.empty());
}

TEST_CASE("agent: the system prompt explains the workflow and both kinds of time") {
  agent::PromptContext ctx;
  ctx.workspace = "/tmp/work";
  ctx.project_hint = "/tmp/work/p.json";
  ctx.project_open = true;
  const std::string prompt = agent::system_prompt(ctx);
  CHECK_THAT(prompt, ContainsSubstring("/tmp/work/p.json"));
  CHECK_THAT(prompt, ContainsSubstring("SOURCE file"));
  CHECK_THAT(prompt, ContainsSubstring("OUTPUT timeline"));
  CHECK_THAT(prompt, ContainsSubstring("preview_frame"));
  CHECK_THAT(prompt, ContainsSubstring("no shell"));

  agent::PromptContext dry = ctx;
  dry.can_export = false;
  CHECK_THAT(agent::system_prompt(dry), ContainsSubstring("dry run"));
}

TEST_CASE("agent: errors from the model surface as ProviderError with the HTTP status") {
  Dir dir;
  Harness h(dir);
  h.server.enqueue(json{{"error", {{"message", "model exploded"}}}}.dump(), 500);
  h.user("do something");
  agent::AgentLoop loop(*h.provider, *h.toolbox);
  try {
    loop.run(h.convo);
    FAIL("expected a ProviderError");
  } catch (const agent::ProviderError& e) {
    CHECK(e.status == 500);
    CHECK_THAT(e.what(), ContainsSubstring("model exploded"));
  }
}

TEST_CASE("agent: a 429 is retried and then succeeds") {
  Dir dir;
  Harness h(dir);
  h.server.enqueue(json{{"error", {{"message", "slow down"}}}}.dump(), 429);
  h.sse(sse_text("after the retry"));
  h.user("do something");
  agent::AgentLoop loop(*h.provider, *h.toolbox);
  const auto result = loop.run(h.convo);
  CHECK(result.final_text == "after the retry");
  CHECK(h.server.request_count() == 2);
}
