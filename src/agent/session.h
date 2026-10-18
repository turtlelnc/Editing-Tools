#pragma once
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "agent/message.h"

namespace et::agent {

struct SessionInfo {
  std::string id, title, created, updated;
  size_t messages = 0;
};

// An append-only conversation log on disk: <root>/sessions/<id>/session.jsonl plus images/<n>.<ext>.
// Images are stored as files (not inline base64) so long sessions stay cheap to load and inspect.
class SessionStore {
 public:
  // $EDITING_TOOLS_HOME, else ~/.editing-tools. Sessions live under sessions/.
  static std::filesystem::path default_root();

  static SessionStore create(const std::filesystem::path& root, const std::string& title);
  static std::optional<SessionStore> open_dir(const std::filesystem::path& dir);

  // Resolves an id ("20261018-174500-ab12") or "last" to a session directory under `root`.
  static std::optional<std::filesystem::path> locate(const std::filesystem::path& root, const std::string& id_or_last);
  static std::vector<SessionInfo> list(const std::filesystem::path& root);

  const std::string& id() const { return id_; }
  const std::filesystem::path& dir() const { return dir_; }
  const std::string& title() const { return title_; }
  const std::vector<Message>& messages() const { return messages_; }
  std::vector<Message>& messages() { return messages_; }

  // Appends one message to the log (and writes any images to their own files first).
  void append(const Message& message);
  void set_title(const std::string& title);

  // Small key/value notes kept with the session (workspace, project path) so --resume can reopen them.
  void set_config(const std::string& key, const std::string& value);
  const std::string& config(const std::string& key) const;

 private:
  void write_metadata();
  void open_log();
  SessionStore(std::filesystem::path dir, std::string id) : dir_(std::move(dir)), id_(std::move(id)) {}

  std::filesystem::path dir_;
  std::string id_, title_, created_;
  std::map<std::string, std::string> config_;
  std::vector<Message> messages_;
  std::ofstream log_;
  size_t image_seq_ = 0;
};

// exp/base64 helpers shared with the providers.
std::string read_file_b64(const std::filesystem::path& file);
std::string image_extension(const std::string& mime);
std::string mime_for_extension(const std::string& ext);

}  // namespace et::agent
