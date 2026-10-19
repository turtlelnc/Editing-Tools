#include "agent/session.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <sstream>

#include "util/base64.h"

namespace et::agent {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string now_iso() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::string new_id() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  char day[16], clock[16];
  std::strftime(day, sizeof day, "%Y%m%d", &tm);
  std::strftime(clock, sizeof clock, "%H%M%S", &tm);
  static std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<int> hex(0, 15);
  std::string suffix;
  for (int i = 0; i < 4; ++i) suffix += "0123456789abcdef"[hex(rng)];
  return std::string(day) + "-" + clock + "-" + suffix;
}

const char* role_wire(Message::Role r) {
  switch (r) {
    case Message::Role::System: return "system";
    case Message::Role::User: return "user";
    case Message::Role::Assistant: return "assistant";
    case Message::Role::Tool: return "tool";
  }
  return "user";
}

Message::Role role_from_wire(const std::string& s) {
  if (s == "system") return Message::Role::System;
  if (s == "assistant") return Message::Role::Assistant;
  if (s == "tool") return Message::Role::Tool;
  return Message::Role::User;
}

}  // namespace

std::string image_extension(const std::string& mime) {
  if (mime == "image/png") return ".png";
  if (mime == "image/webp") return ".webp";
  if (mime == "image/gif") return ".gif";
  return ".jpg";
}

std::string mime_for_extension(const std::string& ext) {
  if (ext == ".png") return "image/png";
  if (ext == ".webp") return "image/webp";
  if (ext == ".gif") return "image/gif";
  return "image/jpeg";
}

std::string read_file_b64(const fs::path& file) {
  std::ifstream f(file, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return util::base64(s.str());
}

fs::path SessionStore::default_root() {
  if (const char* e = std::getenv("EDITING_TOOLS_HOME"); e && *e) return fs::path(e);
  const char* home = std::getenv("HOME");
  if (!home || !*home) return fs::path(".editing-tools");
  return fs::path(home) / ".editing-tools";
}

SessionStore SessionStore::create(const fs::path& root, const std::string& title) {
  const fs::path sessions = root / "sessions";
  std::error_code ec;
  fs::create_directories(sessions, ec);

  std::string id = new_id();
  fs::path dir = sessions / id;
  for (int attempt = 0; fs::exists(dir, ec) && attempt < 20; ++attempt) {  // two sessions in the same second
    id = new_id();
    dir = sessions / id;
  }
  fs::create_directories(dir / "images", ec);
  if (ec) throw std::runtime_error("cannot create session directory " + dir.string() + ": " + ec.message());

  SessionStore store(dir, id);
  store.created_ = now_iso();
  store.title_ = title;
  store.open_log();
  store.write_metadata();
  return store;
}

std::optional<SessionStore> SessionStore::open_dir(const fs::path& dir) {
  if (!fs::is_regular_file(dir / "session.jsonl")) return std::nullopt;
  SessionStore store(dir, dir.filename().string());
  std::ifstream in(dir / "session.jsonl");
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    json j = json::parse(line, nullptr, false);
    if (!j.is_object()) continue;
    const std::string type = j.value("type", "message");
    if (type == "meta") {
      store.title_ = j.value("title", "");
      store.created_ = j.value("created", "");
      continue;
    }
    if (type == "title") {
      store.title_ = j.value("title", "");
      continue;
    }
    if (type == "config") {
      store.config_[j.value("key", "")] = j.value("value", "");
      continue;
    }
    Message m;
    m.role = role_from_wire(j.value("role", "user"));
    m.text = j.value("text", "");
    m.call_id = j.value("call_id", "");
    m.tool_name = j.value("tool_name", "");
    m.is_error = j.value("is_error", false);
    for (const auto& c : j.value("calls", json::array())) {
      ToolCall call;
      call.id = c.value("id", "");
      call.name = c.value("name", "");
      call.arguments = c.value("arguments", json::object());
      m.calls.push_back(std::move(call));
    }
    for (const auto& rel : j.value("images", json::array())) {
      const fs::path file = dir / rel.get<std::string>();
      if (!fs::is_regular_file(file)) continue;
      m.images.push_back({mime_for_extension(file.extension().string()), read_file_b64(file)});
    }
    store.messages_.push_back(std::move(m));
  }
  // Keep new image names clear of the ones already on disk.
  std::error_code ec;
  for (auto& entry : fs::directory_iterator(dir / "images", ec)) {
    const std::string name = entry.path().stem().string();
    if (name.size() == 5 && name.find_first_not_of("0123456789") == std::string::npos)
      store.image_seq_ = std::max(store.image_seq_, static_cast<size_t>(std::stoul(name)) + 1);
  }
  store.open_log();
  return store;
}

void SessionStore::open_log() {
  log_.open(dir_ / "session.jsonl", std::ios::app);
  if (!log_) throw std::runtime_error("cannot write session log in " + dir_.string());
}

std::optional<fs::path> SessionStore::locate(const fs::path& root, const std::string& id_or_last) {
  std::error_code ec;
  if (!id_or_last.empty() && fs::is_directory(id_or_last, ec)) return fs::path(id_or_last);  // an explicit folder
  const fs::path sessions = root / "sessions";
  if (id_or_last == "last" || id_or_last.empty()) {
    fs::path best;
    std::string best_id;
    for (auto& e : fs::directory_iterator(sessions, ec)) {
      if (!e.is_directory()) continue;
      const std::string name = e.path().filename().string();
      if (name > best_id) {
        best_id = name;
        best = e.path();
      }
    }
    if (best.empty()) return std::nullopt;
    return best;
  }
  const fs::path dir = sessions / id_or_last;
  if (!fs::is_directory(dir, ec)) return std::nullopt;
  return dir;
}

std::vector<SessionInfo> SessionStore::list(const fs::path& root) {
  std::vector<SessionInfo> out;
  std::error_code ec;
  const fs::path sessions = root / "sessions";
  if (!fs::is_directory(sessions, ec)) return out;
  for (auto& e : fs::directory_iterator(sessions, ec)) {
    if (!e.is_directory()) continue;
    SessionInfo info;
    info.id = e.path().filename().string();
    std::ifstream in(e.path() / "session.jsonl");
    std::string line;
    while (std::getline(in, line)) {
      json j = json::parse(line, nullptr, false);
      if (!j.is_object()) continue;
      const std::string type = j.value("type", "message");
      if (type == "meta") {
        info.title = j.value("title", "");
        info.created = j.value("created", "");
      } else if (type == "title") {
        info.title = j.value("title", "");
      } else if (type == "message") {
        ++info.messages;
      }
    }
    const auto stamp = fs::last_write_time(e.path() / "session.jsonl", ec);
    if (!ec) {
      info.updated = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(stamp.time_since_epoch()).count());
    }
    out.push_back(std::move(info));
  }
  std::sort(out.begin(), out.end(), [](const SessionInfo& a, const SessionInfo& b) { return a.id > b.id; });
  return out;
}

void SessionStore::write_metadata() {
  json meta = {{"type", "meta"}, {"id", id_}, {"title", title_}, {"created", created_}};
  log_ << meta.dump() << '\n' << std::flush;
}

void SessionStore::set_title(const std::string& title) {
  title_ = title;
  log_ << json{{"type", "title"}, {"title", title}}.dump() << '\n' << std::flush;
}

void SessionStore::set_config(const std::string& key, const std::string& value) {
  config_[key] = value;
  log_ << json{{"type", "config"}, {"key", key}, {"value", value}}.dump() << '\n' << std::flush;
}

const std::string& SessionStore::config(const std::string& key) const {
  static const std::string empty;
  auto it = config_.find(key);
  return it == config_.end() ? empty : it->second;
}

void SessionStore::append(const Message& message) {
  json j = {{"type", "message"}, {"role", role_wire(message.role)}, {"text", message.text}};
  if (!message.call_id.empty()) j["call_id"] = message.call_id;
  if (!message.tool_name.empty()) j["tool_name"] = message.tool_name;
  if (message.is_error) j["is_error"] = true;
  if (!message.calls.empty()) {
    json calls = json::array();
    for (const auto& c : message.calls) calls.push_back({{"id", c.id}, {"name", c.name}, {"arguments", c.arguments}});
    j["calls"] = std::move(calls);
  }
  if (!message.images.empty()) {
    json rels = json::array();
    for (const auto& img : message.images) {
      const size_t seq = image_seq_++;
      char name[32];
      std::snprintf(name, sizeof name, "%05zu%s", seq, image_extension(img.mime).c_str());
      const fs::path rel = fs::path("images") / name;
      const std::string raw = util::unbase64(img.data);  // Image.data is base64; the file holds the bytes
      std::ofstream out(dir_ / rel, std::ios::binary);
      out.write(raw.data(), static_cast<std::streamsize>(raw.size()));
      out.close();
      rels.push_back(rel.string());
    }
    j["images"] = std::move(rels);
  }
  log_ << j.dump() << '\n' << std::flush;
  messages_.push_back(message);
}

}  // namespace et::agent
