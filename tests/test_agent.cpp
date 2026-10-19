#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "agent/loop.h"
#include "agent/openai_chat.h"
#include "agent/policy.h"
#include "agent/prompt.h"
#include "agent/review.h"
#include "agent/session.h"
#include "agent/toolbox.h"
#include "mock_http.h"
#include "process.h"
#include "tools/all.h"
#include "tools/media.h"
#include "util/base64.h"
#include "util/utf8.h"

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

std::string sse_usage(long input, long output) {
  const json chunk = {{"choices", json::array()}, {"usage", {{"prompt_tokens", input}, {"completion_tokens", output}}}};
  return "data: " + chunk.dump() + "\n\n";
}

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

  // Speed is spelled out, and the timeline length is computed with it.
  json sped = project;
  sped["clips"][0]["speed"] = 2.0;
  const json compact3 = agent::compact_project(sped);
  CHECK(compact3["clips"][0]["speed"] == 2.0);
  CHECK(compact3["clips"][0]["length"] == 2.0);
  CHECK(compact["clips"][0]["length"] == 4.0);
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
  CHECK_THAT(agent::system_prompt(dry), ContainsSubstring("render_export is disabled"));
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

namespace {

// Sets an environment variable for the duration of a test case (the CLI tests point EDITING_TOOLS_HOME at a temp
// folder so they never touch the real ~/.editing-tools).
struct EnvGuard {
  std::string name, old;
  bool had = false;
  EnvGuard(const char* key, const std::string& value) : name(key) {
    const char* p = std::getenv(key);
    had = p != nullptr;
    if (had) old = p;
    setenv(key, value.c_str(), 1);
  }
  ~EnvGuard() {
    if (had) setenv(name.c_str(), old.c_str(), 1);
    else unsetenv(name.c_str());
  }
};

std::vector<json> parse_events(const std::string& text) {
  std::vector<json> events;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
    json j = json::parse(line, nullptr, false);
    REQUIRE_FALSE(j.is_discarded());
    events.push_back(std::move(j));
  }
  return events;
}

}  // namespace

TEST_CASE("agent cli: version, usage errors and session listing") {
  const fs::path root = fs::temp_directory_path() / ("et_agent_cli_" + std::to_string(::getpid()));
  fs::remove_all(root);
  EnvGuard home("EDITING_TOOLS_HOME", root.string());

  const auto version = et::run({EDITING_AGENT_BIN, "--version"});
  CHECK(version.exit_code == 0);
  CHECK_THAT(version.out, ContainsSubstring("editing-agent"));

  const auto no_task = et::run({EDITING_AGENT_BIN, "--provider", "openai", "--model", "m", "--api-key", "test"});
  CHECK(no_task.exit_code == 2);
  CHECK_THAT(no_task.err, ContainsSubstring("no task given"));

  const auto bad_provider = et::run({EDITING_AGENT_BIN, "--provider", "nope", "--model", "m", "hello"});
  CHECK(bad_provider.exit_code == 2);
  CHECK_THAT(bad_provider.err, ContainsSubstring("unknown provider"));

  const auto login = et::run({EDITING_AGENT_BIN, "login", "--provider", "openai"});
  CHECK(login.exit_code == 2);
  CHECK_THAT(login.err, ContainsSubstring("only applies to --provider codex"));

  // Bad usage must not leave an empty session behind.
  CHECK(agent::SessionStore::list(root).empty());

  { auto store = agent::SessionStore::create(root, "a saved edit"); }
  {
    const auto listed = et::run({EDITING_AGENT_BIN, "sessions", "--json"});
    CHECK(listed.exit_code == 0);
    const json arr = json::parse(listed.out);
    REQUIRE(arr.is_array());
    REQUIRE(arr.size() == 1);
    CHECK(arr[0]["title"] == "a saved edit");
  }
  std::error_code ec;
  fs::remove_all(root, ec);
}

TEST_CASE("agent cli: one-shot run against a scripted model, then resume the session") {
  Dir dir;
  test::MockServer server;
  const fs::path root = dir.path / "home";
  EnvGuard home("EDITING_TOOLS_HOME", root.string());

  server.enqueue(sse_text("All done from the CLI."), 200, kSse);
  const auto first = et::run({EDITING_AGENT_BIN, "--provider", "openai", "--model", "test-model", "--base-url",
                              server.base_url(), "--workspace", dir.path.string(), "--json", "say hello"});
  CHECK(first.exit_code == 0);
  bool saw_start = false, saw_final = false;
  for (const auto& event : parse_events(first.out)) {
    if (event.value("event", "") == "start") saw_start = true;
    if (event.value("event", "") == "final") {
      saw_final = true;
      CHECK(event["text"] == "All done from the CLI.");
      CHECK(event["session"].is_string());
    }
  }
  CHECK(saw_start);
  CHECK(saw_final);
  CHECK(server.request_count() == 1);
  REQUIRE_THAT(server.request(0).body, ContainsSubstring("say hello"));

  const auto sessions = agent::SessionStore::list(root);
  REQUIRE(sessions.size() == 1);
  CHECK(sessions[0].title == "say hello");
  CHECK(sessions[0].messages >= 2);

  // Resuming restores the history and keeps appending to the same session.
  server.enqueue(sse_text("Continued."), 200, kSse);
  const auto second = et::run({EDITING_AGENT_BIN, "--provider", "openai", "--model", "test-model", "--base-url",
                               server.base_url(), "--workspace", dir.path.string(), "--resume", "last", "--json",
                               "keep going"});
  CHECK(second.exit_code == 0);
  CHECK_THAT(server.request(1).body, ContainsSubstring("All done from the CLI."));
  const auto after = agent::SessionStore::list(root);
  REQUIRE(after.size() == 1);
  CHECK(after[0].id == sessions[0].id);
  CHECK(after[0].messages > sessions[0].messages);
}

TEST_CASE("agent: review_edit extracts frames and reports issues") {
  Dir dir;
  Harness h(dir);
  CHECK_FALSE(h.toolbox->execute({"c1", "create_project", {{"project_path", dir("p.json")}, {"width", 160}, {"height", 120}, {"fps", 10}}}).is_error);
  CHECK_FALSE(h.toolbox->execute({"c2", "add_clip", {{"path", dir.video}}}).is_error);

  h.sse(sse_text(R"({"issues":[{"severity":"high","at":1.5,"message":"subtitle covers a face"},)"
                 R"({"severity":"low","at":-1,"message":"music is a little loud"}]})"));
  const auto project = h.session->get().to_json();
  const auto issues = agent::review_edit(*h.provider, project, "make it warm", "subtitles", 4);

  REQUIRE(issues.size() == 2);
  CHECK(issues[0].severity == "high");
  CHECK(issues[0].at == 1.5);
  CHECK_THAT(issues[0].message, ContainsSubstring("subtitle"));
  const json body = json::parse(h.server.last_request().body);
  CHECK_FALSE(body.contains("tools"));  // the reviewer cannot change anything
  CHECK_THAT(body.dump(), ContainsSubstring("data:image/jpeg;base64,"));  // it really looked at frames
  CHECK_THAT(body.dump(), ContainsSubstring("contact sheet"));
}

TEST_CASE("agent: review_edit refuses an empty timeline") {
  Dir dir;
  Harness h(dir);
  model::Project empty;
  CHECK_THROWS_AS(agent::review_edit(*h.provider, empty.to_json(), "task", "", 3), std::invalid_argument);
}

TEST_CASE("agent: a scripted model drives a full edit and renders a real file") {
  Dir dir;
  Harness h(dir);
  const std::string project = dir("p.json"), output = dir("out.mp4");

  h.sse(sse_call("c1", "create_project", {{"project_path", project}, {"width", 160}, {"height", 120}, {"fps", 10}}));
  h.sse(sse_call("c2", "add_clip", {{"path", dir.video}}));
  h.sse(sse_call("c3", "color_adjust", {{"warmth", 0.25}, {"contrast", 1.1}}));
  h.sse(sse_call("c4", "render_export", {{"output_path", output}}));
  h.sse(sse_text("Done: warm grade applied, exported to out.mp4."));
  h.user("Grade it warm and export.");

  agent::AgentLoop loop(*h.provider, *h.toolbox);
  const auto result = loop.run(h.convo);

  REQUIRE(result.final_text == "Done: warm grade applied, exported to out.mp4.");
  REQUIRE(fs::exists(output));
  CHECK(fs::file_size(output) > 1000);
  const json probed = tools::probe_media(output);
  CHECK(probed["duration_sec"].get<double>() > 2.5);
  CHECK(probed["duration_sec"].get<double>() < 3.6);

  // The grade really reached the project that was saved along the way.
  std::ifstream f(project);
  REQUIRE(f);
  const json pj = json::parse(f);
  CHECK(pj["color"]["warmth"] == 0.25);
}

TEST_CASE("agent cli: --no-export refuses render_export even when the model asks") {
  Dir dir;
  test::MockServer server;
  const fs::path root = dir.path / "home";
  EnvGuard home("EDITING_TOOLS_HOME", root.string());
  const std::string project = dir("p.json"), output = dir("out.mp4");

  server.enqueue(sse_call("c1", "create_project", {{"project_path", project}, {"width", 160}, {"height", 120}, {"fps", 10}}), 200, kSse);
  server.enqueue(sse_call("c2", "add_clip", {{"path", dir.video}}), 200, kSse);
  server.enqueue(sse_call("c3", "render_export", {{"output_path", output}}), 200, kSse);
  server.enqueue(sse_text("I did not export (dry run)."), 200, kSse);

  const auto r = et::run({EDITING_AGENT_BIN, "--provider", "openai", "--model", "m", "--base-url", server.base_url(),
                          "--workspace", dir.path.string(), "--no-export", "--json", "grade and export"});
  CHECK(r.exit_code == 0);
  CHECK_FALSE(fs::exists(output));
  REQUIRE(server.request_count() == 4);
  // The model was told why the render was refused, as a normal tool error it can react to.
  const json body = json::parse(server.request(3).body);
  bool saw_refusal = false;
  for (const auto& m : body["messages"])
    if (m.value("role", "") == "tool" && m["content"].get<std::string>().find("--no-export") != std::string::npos)
      saw_refusal = true;
  CHECK(saw_refusal);
}

TEST_CASE("agent: text helpers never split UTF-8") {
  using et::util::valid_utf8;
  const std::string chinese = "把 clip.mp4 做成项目，整段调成偏冷，预览第 1 秒确认。";
  CHECK(valid_utf8(chinese));
  const std::string cut = et::util::truncate_utf8(chinese, 100);
  CHECK(cut.size() <= 100);
  CHECK(valid_utf8(cut));

  // A byte cut that would land inside a code point is pulled back to the boundary.
  CHECK(et::util::truncate_utf8("你好", 4) == "你");  // 你 = 3 bytes, 好 starts at byte 3
  CHECK(et::util::truncate_utf8("你好", 3) == "你");
  CHECK(et::util::truncate_utf8("abc", 100) == "abc");
  CHECK(et::util::utf8_complete_prefix("你好") == 6);
  CHECK(et::util::utf8_complete_prefix(std::string("你").substr(0, 2)) == 0);  // truncated sequence
  CHECK(valid_utf8(et::util::sanitize_utf8(std::string("\xE4\xBD"))));

  // JSON holding a malformed byte is repaired instead of throwing.
  const json bad = {{"text", std::string(1, static_cast<char>(0xAF))}};
  const std::string dumped = et::util::dump_json(bad);
  CHECK(valid_utf8(dumped));
  CHECK_THAT(dumped, ContainsSubstring("\xEF\xBF\xBD"));
}

TEST_CASE("agent cli: a long non-ASCII task is stored without breaking the session log") {
  Dir dir;
  test::MockServer server;
  const fs::path root = dir.path / "home";
  EnvGuard home("EDITING_TOOLS_HOME", root.string());
  server.enqueue(sse_text("好的，已完成。"), 200, kSse);

  std::string task;
  for (int i = 0; i < 20; ++i) task += "把这段素材调成偏冷色调，";

  const auto r = et::run({EDITING_AGENT_BIN, "--provider", "openai", "--model", "m", "--base-url", server.base_url(),
                          "--workspace", dir.path.string(), "--json", task});
  CHECK(r.exit_code == 0);
  CHECK(server.request_count() == 1);

  // This used to throw "incomplete UTF-8 string" when the title was cut in the middle of a character.
  const auto sessions = agent::SessionStore::list(root);
  REQUIRE(sessions.size() == 1);
  CHECK(et::util::valid_utf8(sessions[0].title));
  CHECK(sessions[0].title.size() <= 100);
  CHECK_THAT(sessions[0].title, ContainsSubstring("把这段素材"));
}

TEST_CASE("agent: a token budget stops the turn with a wrap-up instead of more tools") {
  Dir dir;
  Harness h(dir);
  const std::string project = dir("p.json");
  h.sse(sse_call("c1", "create_project", {{"project_path", project}}) + sse_usage(900, 300));
  h.sse(sse_text("Stopping: the token budget for this turn is used up.") + sse_usage(500, 40));
  h.user("make a project");
  agent::LoopConfig cfg;
  cfg.max_tokens = 1000;
  agent::AgentLoop loop(*h.provider, *h.toolbox, cfg);
  const auto result = loop.run(h.convo);
  CHECK(result.hit_budget);
  CHECK_FALSE(result.hit_max_steps);
  CHECK(result.steps == 2);
  CHECK_THAT(result.final_text, ContainsSubstring("budget"));
  CHECK_FALSE(fs::exists(project));  // the refused tool call was never run
  REQUIRE(h.server.request_count() == 2);
  const json wrap = json::parse(h.server.request(1).body);
  CHECK_FALSE(wrap.contains("tools"));  // the wrap-up cannot call tools
  // The log stays valid: the refused call got an answer.
  bool saw_skipped = false;
  for (const auto& m : h.convo)
    if (m.role == agent::Message::Role::Tool && m.is_error && m.text.find("budget") != std::string::npos)
      saw_skipped = true;
  CHECK(saw_skipped);
}

TEST_CASE("agent: repeating the same failing call earns a hint, and enough failures stop the turn") {
  Dir dir;
  Harness h(dir);
  h.sse(sse_call("c1", "delete_clip", {{"clip_id", "nope"}}));
  h.sse(sse_call("c2", "delete_clip", {{"clip_id", "nope"}}));
  h.sse(sse_call("c3", "delete_clip", {{"clip_id", "nope"}}));
  h.sse(sse_text("I keep failing; let me stop and ask."));
  h.user("delete that clip");

  agent::LoopConfig cfg;
  cfg.max_consecutive_errors = 3;
  agent::AgentLoop loop(*h.provider, *h.toolbox, cfg);
  const auto result = loop.run(h.convo);

  CHECK(result.hit_error_limit);
  CHECK_THAT(result.final_text, ContainsSubstring("stop"));
  REQUIRE(h.server.request_count() == 4);  // three failures and a wrap-up
  const json second = json::parse(h.server.request(1).body);
  const json third = json::parse(h.server.request(2).body);
  const std::string third_text = third["messages"].back()["content"].get<std::string>();
  CHECK_THAT(third_text, ContainsSubstring("failed 2 times"));
  // The wrap-up request carries the last failure and no tools.
  const json wrap = json::parse(h.server.request(3).body);
  CHECK_FALSE(wrap.contains("tools"));
  CHECK_THAT(second.dump(), ContainsSubstring("nope"));
}

TEST_CASE("agent cli: --self-review refuses an export when the reviewer finds a problem") {
  Dir dir;
  test::MockServer server;
  const fs::path root = dir.path / "home";
  EnvGuard home("EDITING_TOOLS_HOME", root.string());
  const std::string project = dir("p.json"), output = dir("out.mp4");

  server.enqueue(sse_call("c1", "create_project", {{"project_path", project}, {"width", 160}, {"height", 120}, {"fps", 10}}), 200, kSse);
  server.enqueue(sse_call("c2", "add_clip", {{"path", dir.video}}), 200, kSse);
  server.enqueue(sse_call("c3", "render_export", {{"output_path", output}}), 200, kSse);
  // The self-review call: it gets frames and no tools; report one issue.
  server.enqueue(sse_text(R"({"issues":[{"severity":"high","at":1.0,"message":"the clip is far too dark"}]})"), 200, kSse);
  server.enqueue(sse_text("The review found the clip is too dark; I will fix it before exporting."), 200, kSse);

  const auto r = et::run({EDITING_AGENT_BIN, "--provider", "openai", "--model", "m", "--base-url", server.base_url(),
                          "--workspace", dir.path.string(), "--self-review", "--json", "make it brighter and export"});
  CHECK(r.exit_code == 0);
  CHECK_FALSE(fs::exists(output));  // the export was refused
  REQUIRE(server.request_count() == 5);
  // The reviewer saw a contact sheet, not the whole toolset.
  const json review = json::parse(server.request(3).body);
  CHECK_FALSE(review.contains("tools"));
  CHECK_THAT(review.dump(), ContainsSubstring("data:image/jpeg;base64,"));
  // The model was told what to fix.
  const json after = json::parse(server.request(4).body);
  bool saw_refusal = false;
  for (const auto& m : after["messages"])
    if (m.value("role", "") == "tool" && m["content"].get<std::string>().find("self-review found issues") != std::string::npos)
      saw_refusal = true;
  CHECK(saw_refusal);
}

TEST_CASE("agent cli: --self-review lets a clean export through") {
  Dir dir;
  test::MockServer server;
  const fs::path root = dir.path / "home";
  EnvGuard home("EDITING_TOOLS_HOME", root.string());
  const std::string project = dir("p.json"), output = dir("out.mp4");

  server.enqueue(sse_call("c1", "create_project", {{"project_path", project}, {"width", 160}, {"height", 120}, {"fps", 10}}), 200, kSse);
  server.enqueue(sse_call("c2", "add_clip", {{"path", dir.video}}), 200, kSse);
  server.enqueue(sse_call("c3", "render_export", {{"output_path", output}}), 200, kSse);
  server.enqueue(sse_text(R"({"issues":[]})"), 200, kSse);
  server.enqueue(sse_text("Exported."), 200, kSse);

  const auto r = et::run({EDITING_AGENT_BIN, "--provider", "openai", "--model", "m", "--base-url", server.base_url(),
                          "--workspace", dir.path.string(), "--self-review", "--json", "export the clip"});
  CHECK(r.exit_code == 0);
  REQUIRE(fs::exists(output));
  REQUIRE(server.request_count() == 5);
  CHECK_THAT(server.request(3).body, ContainsSubstring("contact sheet"));
}

