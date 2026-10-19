#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "agent/codex_auth.h"
#include "agent/loop.h"
#include "agent/policy.h"
#include "agent/prompt.h"
#include "agent/providers.h"
#include "agent/review.h"
#include "agent/session.h"
#include "agent/toolbox.h"
#include "mcp/server.h"
#include "model/timeline.h"
#include "tools/all.h"
#include "tools/session.h"
#include "util/utf8.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr const char* kVersion = "editing-agent 0.1.0";

const char* kUsage = R"(editing-agent — an AI video editor that uses the editing-tools.

Usage:
  editing-agent [options] "task"        run one task
  editing-agent chat [options]          keep a conversation going (one task per line, /exit to leave)
  editing-agent login                   sign in with a ChatGPT subscription (--provider codex)
  editing-agent sessions                list saved sessions

Options:
  --provider openai|anthropic|codex   model backend (default: openai)
  --model NAME                        model id; required for openai, has a default for the others
  --base-url URL                      override the API endpoint (e.g. a local OpenAI-compatible server)
  --api-key KEY                       API key (also read from OPENAI_API_KEY / ANTHROPIC_API_KEY)
  --proxy URL                         send model requests through this HTTP proxy (also applies to login)
  --project FILE                      project JSON to open, or to create if it does not exist yet
  --workspace DIR                     folder the agent may read and write (default: current directory)
  --allow DIR                         extra folder the agent may use (repeatable: footage, LUTs, output)
  --overwrite                         allow replacing an existing output file
  --no-export                         dry run: no render_export (previews are fine)
  --max-steps N                       model calls per turn before a wrap-up (default 40)
  --resume ID|last                    continue a saved session (the workspace/project are restored)
  --notes FILE                        project memory appended to the system prompt
  --json                              machine-readable JSONL events on stdout
  --quiet, -q                         no progress lines on stderr
  --yes, -y                           do not ask before rendering in chat mode
  --help, -h                          this text
  --version                           version

Environment: EDITING_TOOLS_HOME (state directory, default ~/.editing-tools),
EDITING_TOOLS_FFMPEG (an ffmpeg with libass, for burned-in subtitles).
)";

struct UsageError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Options {
  std::string mode = "run";  // run | chat | login | sessions
  std::string provider = "openai", model, base_url, api_key, proxy, project, workspace, resume, notes;
  bool provider_given = false;
  std::vector<std::string> allow, task;
  int max_steps = 40;
  bool overwrite = false, no_export = false, json = false, quiet = false, yes = false, help = false, version = false;
};

std::string trim(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s;
}

Options parse(int argc, char** argv) {
  Options o;
  std::vector<std::string> args(argv + 1, argv + argc);
  size_t i = 0;
  if (!args.empty() && (args[0] == "chat" || args[0] == "login" || args[0] == "sessions" || args[0] == "run")) {
    o.mode = args[0] == "run" ? "run" : args[0];
    i = 1;
  }
  for (; i < args.size(); ++i) {
    const std::string a = args[i];
    auto need = [&](const char* name) -> std::string {
      if (i + 1 >= args.size()) throw UsageError(std::string("--") + name + " needs a value");
      return args[++i];
    };
    if (a == "-h" || a == "--help") {
      o.help = true;
      continue;
    }
    if (a == "--version") {
      o.version = true;
      continue;
    }
    if (a == "-q" || a == "--quiet") {
      o.quiet = true;
      continue;
    }
    if (a == "-y" || a == "--yes") {
      o.yes = true;
      continue;
    }
    if (a.rfind("--", 0) == 0) {
      std::string name = a.substr(2), value;
      bool has_value = false;
      if (auto eq = name.find('='); eq != std::string::npos) {
        value = name.substr(eq + 1);
        name = name.substr(0, eq);
        has_value = true;
      }
      auto val = [&]() { return has_value ? value : need(name.c_str()); };
      if (name == "provider") {
        o.provider = val();
        o.provider_given = true;
      }
      else if (name == "model") o.model = val();
      else if (name == "base-url") o.base_url = val();
      else if (name == "api-key") o.api_key = val();
      else if (name == "proxy") o.proxy = val();
      else if (name == "project") o.project = val();
      else if (name == "workspace") o.workspace = val();
      else if (name == "allow") o.allow.push_back(val());
      else if (name == "max-steps") o.max_steps = std::max(1, std::atoi(val().c_str()));
      else if (name == "resume") o.resume = val();
      else if (name == "notes") o.notes = val();
      else if (name == "overwrite") o.overwrite = true;
      else if (name == "no-export") o.no_export = true;
      else if (name == "json") o.json = true;
      else throw UsageError("unknown option --" + name);
      continue;
    }
    o.task.push_back(a);
  }
  return o;
}

std::string read_text_file(const std::string& file, size_t limit = 40000) {
  std::ifstream f(file, std::ios::binary);
  if (!f) throw UsageError("cannot read " + file);
  std::ostringstream s;
  s << f.rdbuf();
  std::string text = s.str();
  if (text.size() > limit) text = text.substr(0, limit) + "\n…[truncated]";
  return text;
}

// ---------------------------------------------------------------------------------------------------------------
// Output. In JSON mode stdout is a stream of JSONL events; otherwise assistant text streams to stdout and progress
// goes to stderr, so `editing-agent ... > out.txt` captures just the answer.

class Sink {
 public:
  Sink(bool json_mode, bool quiet) : json_(json_mode), quiet_(quiet) {}

  void delta(const std::string& s) {
    if (json_) {
      // A delta may end in the middle of a multi-byte character; hold those bytes until the rest arrives.
      pending_text_ += s;
      const size_t complete = et::util::utf8_complete_prefix(pending_text_);
      if (complete == 0) return;
      emit({{"event", "text"}, {"delta", pending_text_.substr(0, complete)}});
      pending_text_.erase(0, complete);
      return;
    }
    std::cout << s << std::flush;
    mid_text_ = true;
  }
  void note(const std::string& line) {
    if (json_) {
      flush_text();
      return emit({{"event", "notice"}, {"text", line}});
    }
    if (quiet_) return;
    end_text();
    std::cerr << line << '\n';
  }
  void tool_start(const et::agent::ToolCall& call) {
    std::string args = et::util::dump_json(call.arguments);
    if (args.size() > 240) args = args.substr(0, 240) + "…";
    if (json_) return emit({{"event", "tool"}, {"name", call.name}, {"arguments", call.arguments}});
    note("  -> " + call.name + " " + args);
  }
  void tool_done(const et::agent::ToolCall& call, const et::agent::ToolResult& r) {
    if (json_) flush_text();
    if (json_)
      return emit({{"event", "tool_result"},
                   {"name", call.name},
                   {"ok", !r.is_error},
                   {"chars", r.text.size()},
                   {"images", r.images.size()},
                   {"text", r.text.size() > 4000 ? r.text.substr(0, 4000) : r.text}});
    if (r.is_error) {
      std::string text = r.text;
      if (text.size() > 400) text = text.substr(0, 400) + "…";
      note("     failed: " + text);
    } else if (!r.images.empty()) {
      note("     ok (" + std::to_string(r.images.size()) + " image" + (r.images.size() == 1 ? "" : "s") + ")");
    }
  }
  void end_text() {
    if (json_) {
      flush_text();
      return;
    }
    if (!json_ && mid_text_) {
      std::cout << "\n" << std::flush;
      mid_text_ = false;
    }
  }
  void final(const et::agent::LoopResult& r, const std::string& session_id) {
    if (json_) {
      flush_text();
      emit({{"event", "final"},
            {"text", r.final_text},
            {"steps", r.steps},
            {"input_tokens", r.input_tokens},
            {"output_tokens", r.output_tokens},
            {"hit_max_steps", r.hit_max_steps},
            {"cancelled", r.cancelled},
            {"session", session_id}});
      return;
    }
    if (!quiet_) {
      std::cerr << "[" << r.steps << " step" << (r.steps == 1 ? "" : "s") << ", " << r.input_tokens << " in / "
                << r.output_tokens << " out tokens";
      if (r.hit_max_steps) std::cerr << ", stopped at the step limit";
      std::cerr << "]\n";
    }
  }
  void error(const std::string& message) {
    if (json_) {
      flush_text();
      return emit({{"event", "error"}, {"message", et::util::sanitize_utf8(message)}});
    }
    std::cerr << "error: " << message << "\n";
  }
  void start(const std::string& session_id, const et::agent::Provider& provider, const std::string& model,
             const fs::path& workspace) {
    if (!json_) return;
    emit({{"event", "start"},
          {"session", session_id},
          {"provider", provider.name()},
          {"model", model},
          {"workspace", workspace.string()}});
  }
  bool json_mode() const { return json_; }

 private:
  // In JSON mode, emit whatever complete text is buffered first (keeps events in order).
  void flush_text() {
    if (pending_text_.empty()) return;
    emit({{"event", "text"}, {"delta", et::util::sanitize_utf8(pending_text_)}});
    pending_text_.clear();
  }
  void emit(const json& j) { std::cout << et::util::dump_json(j) << '\n' << std::flush; }

  bool json_ = false, quiet_ = false, mid_text_ = false;
  std::string pending_text_;
};

// ---------------------------------------------------------------------------------------------------------------
// Ctrl-C: the first one asks the current turn to stop (the in-flight request is aborted); a second one exits.

std::atomic<et::agent::AgentLoop*> g_loop{nullptr};
std::atomic<int> g_interrupts{0};

void on_sigint(int) {
  if (++g_interrupts >= 2) _exit(130);
  if (auto* loop = g_loop.load()) loop->cancel();
}

std::string join(const std::vector<std::string>& parts) {
  std::string out;
  for (const auto& p : parts) {
    if (!out.empty()) out += ' ';
    out += p;
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------------------

int do_login(const Options& o_in) {
  Options o = o_in;
  if (!o.provider_given) o.provider = "codex";  // `editing-agent login` means the ChatGPT flow unless told otherwise
  if (o.provider != "codex") {
    std::cerr << "login only applies to --provider codex; for API providers set OPENAI_API_KEY or ANTHROPIC_API_KEY\n";
    return 2;
  }
  std::string err;
  auto announce = [&](const std::string& text) {
    if (o.json) {
      std::cout << json{{"event", "device_code"}, {"text", text}}.dump() << '\n' << std::flush;
    } else {
      std::cerr << text << "\n";
    }
  };
  if (!et::agent::codex_login(err, announce)) {
    if (o.json) std::cout << json{{"event", "error"}, {"message", err}}.dump() << '\n';
    else std::cerr << "login failed: " << err << "\n";
    return 1;
  }
  const std::string where = et::agent::codex_auth_path().string();
  if (o.json) std::cout << json{{"event", "signed_in"}, {"auth_file", where}}.dump() << '\n';
  else std::cout << "Signed in. Credentials saved to " << where << "\n";
  return 0;
}

int do_sessions(const Options& o) {
  const fs::path root = et::agent::SessionStore::default_root();
  const auto list = et::agent::SessionStore::list(root);
  if (o.json) {
    json out = json::array();
    for (const auto& s : list)
      out.push_back({{"id", s.id}, {"title", s.title}, {"created", s.created}, {"messages", s.messages}});
    std::cout << out.dump(2) << '\n';
    return 0;
  }
  if (list.empty()) {
    std::cout << "No sessions yet (" << (root / "sessions").string() << ").\n";
    return 0;
  }
  std::cout << "Sessions in " << (root / "sessions").string() << ":\n";
  for (const auto& s : list)
    std::cout << "  " << s.id << "  " << s.messages << " msg  " << (s.title.empty() ? "(untitled)" : s.title)
              << "\n";
  return 0;
}

// Everything a turn needs, assembled once.
struct App {
  Options o;
  fs::path workspace;
  std::string project_path;
  bool project_open = false;
  std::shared_ptr<et::tools::Session> session = std::make_shared<et::tools::Session>();
  std::unique_ptr<et::agent::Provider> provider;
  std::unique_ptr<et::agent::ToolBox> toolbox;
  std::unique_ptr<et::agent::AgentLoop> loop;
  std::unique_ptr<et::agent::SessionStore> store;
  std::shared_ptr<std::string> current_task = std::make_shared<std::string>();
  std::shared_ptr<int> reviews = std::make_shared<int>(0);
  std::vector<et::agent::Message> convo;  // the in-memory conversation, mirrored into the session log
  Sink sink{false, false};
};

void persist_config(App& app) {
  if (!app.store) return;
  if (app.toolbox->has_project()) {
    const std::string path = app.session->path;
    if (!path.empty() && app.store->config("project") != path) app.store->set_config("project", path);
  }
  if (app.store->config("workspace") != app.workspace.string())
    app.store->set_config("workspace", app.workspace.string());
}

et::agent::LoopResult run_turn(App& app, const std::string& task) {
  *app.reviews = 0;
  *app.current_task = task;

  et::agent::Message user;
  user.role = et::agent::Message::Role::User;
  user.text = task;
  // On resume the history was loaded from the session store; the log is appended to as the turn grows.
  auto& convo = app.convo;

  et::agent::LoopEvents ev;
  ev.text_delta = [&](const std::string& d) { app.sink.delta(d); };
  ev.notice = [&](const std::string& line) { app.sink.note(line); };
  ev.tool_start = [&](const et::agent::ToolCall& c) { app.sink.tool_start(c); };
  ev.tool_done = [&](const et::agent::ToolCall& c, const et::agent::ToolResult& r) { app.sink.tool_done(c, r); };

  app.store->append(user);
  convo.push_back(user);
  ev.on_message = [&](const et::agent::Message& m) { app.store->append(m); };

  et::agent::LoopResult result;
  try {
    result = app.loop->run(convo, ev);
  } catch (...) {
    persist_config(app);
    throw;
  }
  app.sink.end_text();
  app.sink.final(result, app.store->id());
  persist_config(app);
  return result;
}

int run_mode(const Options& o) {
  App app;
  app.o = o;
  app.sink = Sink(o.json, o.quiet);

  // --- workspace and session ---------------------------------------------------------------------------------
  fs::path resume_dir;
  if (!o.resume.empty()) {
    auto found = et::agent::SessionStore::locate(et::agent::SessionStore::default_root(), o.resume);
    if (!found) throw UsageError("no session named '" + o.resume + "' (see: editing-agent sessions)");
    resume_dir = *found;
  }

  std::string workspace = o.workspace;
  if (workspace.empty() && !resume_dir.empty()) {
    auto store = et::agent::SessionStore::open_dir(resume_dir);
    if (store) workspace = store->config("workspace");
  }
  app.workspace = fs::absolute(workspace.empty() ? fs::current_path() : fs::path(workspace));
  if (!fs::is_directory(app.workspace)) throw UsageError("workspace is not a folder: " + app.workspace.string());

  if (!resume_dir.empty()) {
    auto store = et::agent::SessionStore::open_dir(resume_dir);
    if (!store) throw UsageError("session folder is damaged: " + resume_dir.string());
    app.store = std::make_unique<et::agent::SessionStore>(std::move(*store));
  } else {
    app.store = std::make_unique<et::agent::SessionStore>(
        et::agent::SessionStore::create(et::agent::SessionStore::default_root(), ""));
  }
  app.store->set_config("workspace", app.workspace.string());

  // --- project -----------------------------------------------------------------------------------------------
  std::string project = o.project;
  if (project.empty()) project = app.store->config("project");
  if (!project.empty()) {
    fs::path p = fs::absolute(project);
    if (fs::exists(p)) {
      app.session->open(p.string());
      app.project_path = p.string();
      app.project_open = true;
    } else {
      app.project_path = p.string();
    }
  }

  // --- the sandbox the agent lives in ------------------------------------------------------------------------
  et::agent::PolicyConfig policy_cfg;
  policy_cfg.roots.push_back(app.workspace);
  for (const auto& dir : o.allow) {
    fs::path p = fs::absolute(dir);
    if (!fs::is_directory(p)) throw UsageError("--allow is not a folder: " + p.string());
    policy_cfg.roots.push_back(p);
  }
  if (!app.project_path.empty()) {
    // The user pointed at the project file, so its folder is fair game even outside the workspace.
    policy_cfg.roots.push_back(fs::path(app.project_path).parent_path());
  }
  policy_cfg.overwrite = o.overwrite;

  et::agent::ProviderConfig pc;
  pc.kind = o.provider;
  pc.model = o.model;
  pc.base_url = o.base_url;
  pc.api_key = o.api_key;
  try {
    app.provider = et::agent::make_provider(pc);
  } catch (const std::invalid_argument& e) {
    throw UsageError(e.what());
  }

  try {
    app.toolbox = std::make_unique<et::agent::ToolBox>(app.session, et::agent::Policy(policy_cfg));
  } catch (const std::invalid_argument& e) {
    throw UsageError(e.what());
  }

  if (!app.project_open && !app.project_path.empty()) {
    // A pending project path is a hint for the model; the tools still need the folder to be allowed (done above).
    app.sink.note("project '" + app.project_path + "' does not exist yet; the agent will create it");
  }

  auto provider_ptr = app.provider.get();
  auto toolbox_ptr = app.toolbox.get();
  auto reviews = app.reviews;
  auto current_task = app.current_task;
  auto sink_ptr = &app.sink;
  app.toolbox->add_tool(
      {"review_edit",
       "Ask an independent reviewer for a second opinion on the current edit before exporting. The reviewer has no "
       "tools: it looks at frames sampled from the timeline plus the timeline JSON and reports concrete problems "
       "(color mismatch, subtitles over faces, abrupt cuts) with severity, timeline time and a suggested fix. At most "
       "twice per turn.",
       et::tools::props({{"focus", et::tools::kStr}}),
       [provider_ptr, toolbox_ptr, reviews, current_task, sink_ptr](const json& a) -> json {
         if (*reviews >= 2)
           throw et::mcp::ToolError("review_edit was already used twice this turn; fix what it found and continue");
         ++*reviews;
         sink_ptr->note("reviewing the edit (" + std::to_string(*reviews) + "/2)…");
         auto& project = toolbox_ptr->session().get();
         const auto issues = et::agent::review_edit(*provider_ptr, project.to_json(), *current_task,
                                                    a.value("focus", ""), 6);
         json list = json::array();
         for (const auto& issue : issues)
           list.push_back({{"severity", issue.severity}, {"at", issue.at}, {"message", issue.message}});
         return json{{"issues", list},
                     {"note", issues.empty() ? "The reviewer found no concrete problems." : "Address these before exporting."}};
       }});

  // Ask before replacing a finished file. Chat mode can prompt; a one-shot run was explicit about wanting output.
  app.toolbox->set_approver([&o](const std::string& tool, const json& args, std::string& reason) {
    if (tool != "render_export") return true;
    if (o.no_export) {
      reason = "this run is a --no-export dry run; do not write files. Describe the edit instead.";
      return false;
    }
    if (o.yes || o.mode != "chat") return true;
    if (!isatty(STDIN_FILENO)) return true;  // piped chat cannot answer
    std::cerr << "\nrender " << args.value("output_path", std::string("?")) << "? [y/N] " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return false;
    if (!line.empty() && (line[0] == 'y' || line[0] == 'Y')) return true;
    reason = "the user declined the render; suggest what to change or ask what they want";
    return false;
  });

  // --- prompt and conversation -------------------------------------------------------------------------------
  et::agent::PromptContext prompt_ctx;
  prompt_ctx.workspace = app.workspace;
  prompt_ctx.project_hint = app.project_path;
  prompt_ctx.project_open = app.project_open;
  prompt_ctx.can_export = !o.no_export;
  if (!o.notes.empty()) prompt_ctx.notes = read_text_file(o.notes);
  const std::string prompt = et::agent::system_prompt(prompt_ctx);

  app.convo = app.store->messages();
  if (!app.convo.empty() && app.convo.front().role == et::agent::Message::Role::System) app.convo.erase(app.convo.begin());
  et::agent::Message system;
  system.role = et::agent::Message::Role::System;
  system.text = prompt;
  app.convo.insert(app.convo.begin(), std::move(system));

  et::agent::LoopConfig loop_cfg;
  loop_cfg.max_steps = o.max_steps;
  app.loop = std::make_unique<et::agent::AgentLoop>(*app.provider, *app.toolbox, loop_cfg);

  app.sink.start(app.store->id(), *app.provider, o.model.empty() ? "(default)" : o.model, app.workspace);
  if (!o.json) {
    std::cerr << "editing-agent " << app.store->id() << " · " << app.provider->name() << " · workspace "
              << app.workspace.string();
    if (!app.project_path.empty()) std::cerr << " · project " << app.project_path;
    std::cerr << "\n";
  }

  g_loop.store(app.loop.get());
  struct sigaction sa {};
  sa.sa_handler = on_sigint;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);

  auto finish = [&](int code) {
    g_loop.store(nullptr);
    return code;
  };

  if (o.mode == "run") {
    std::string task = join(o.task);
    if (task.empty() && !isatty(STDIN_FILENO) && !o.help) {
      std::ostringstream s;
      s << std::cin.rdbuf();
      task = trim(s.str());
    }
    if (task.empty()) throw UsageError("no task given (pass one as an argument, or pipe it on stdin)");
    if (app.store->title().empty()) app.store->set_title(et::util::truncate_utf8(task, 100));
    try {
      auto result = run_turn(app, task);
      if (result.cancelled) return finish(130);
      return finish(0);
    } catch (const std::exception& e) {
      if (app.loop->cancelled()) {
        app.sink.end_text();
        app.sink.error("cancelled");
        return finish(130);
      }
      throw;
    }
  }

  // chat
  if (!o.json) {
    std::cerr << "Chat mode: describe what you want edited, one task per line. /exit leaves, Ctrl-C stops a turn.\n";
  }
  while (true) {
    if (isatty(STDIN_FILENO) && !o.json) std::cerr << "\nyou> " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) break;
    line = trim(line);
    if (line.empty()) continue;
    if (line == "/exit" || line == "/quit") break;
    if (line == "/timeline") {
      if (!app.toolbox->has_project()) {
        app.sink.note("no project is open yet");
        continue;
      }
      std::cout << et::util::dump_json(app.session->get().to_json(), 2) << "\n";
      continue;
    }
    if (app.store->title().empty()) app.store->set_title(et::util::truncate_utf8(line, 100));
    g_interrupts.store(0);
    try {
      run_turn(app, line);
    } catch (const std::exception& e) {
      if (app.loop->cancelled()) {
        app.sink.end_text();
        app.sink.error("cancelled");
        continue;
      }
      app.sink.error(e.what());
    }
  }
  return finish(0);
}

}  // namespace

int main(int argc, char** argv) {
  std::ios::sync_with_stdio(false);
  try {
    Options o = parse(argc, argv);
    if (!o.proxy.empty()) {
      // curl (used for every model request) reads these; loopback URLs are explicitly excluded by the client,
      // so local mock servers and local model servers are never sent through the proxy.
      for (const char* name : {"HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy", "ALL_PROXY", "all_proxy"})
        setenv(name, o.proxy.c_str(), 1);
    }
    if (o.help) {
      std::cout << kUsage;
      return 0;
    }
    if (o.version) {
      std::cout << kVersion << "\n";
      return 0;
    }
    if (o.mode == "login") return do_login(o);
    if (o.mode == "sessions") return do_sessions(o);
    return run_mode(o);
  } catch (const UsageError& e) {
    std::cerr << "error: " << e.what() << "\n\n" << kUsage;
    return 2;
  } catch (const et::agent::ProviderError& e) {
    std::cerr << "error: " << e.what() << "\n";
    if (e.status == 401) std::cerr << "(check the API key; for --provider codex run: editing-agent login)\n";
    if (e.status == 429) std::cerr << "(rate limited; wait a moment and try again)\n";
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
