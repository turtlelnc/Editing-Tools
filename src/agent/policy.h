#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace et::agent {

struct PolicyConfig {
  std::vector<std::filesystem::path> roots;  // folders the agent may read from and write into; roots[0] is the workspace
  bool overwrite = false;                    // allow replacing files that already exist
};

// What the agent is allowed to touch. It has no shell, so file paths in tool arguments are the only way out.
class Policy {
 public:
  explicit Policy(PolicyConfig cfg);

  const std::filesystem::path& workspace() const { return roots_.front(); }

  // Relative paths are taken relative to the workspace.
  std::filesystem::path resolve(const std::string& path) const;
  bool allowed(const std::filesystem::path& absolute_path) const;

  // Rewrites path arguments to absolute paths and checks them. Throws std::invalid_argument with a message the
  // model can act on when a path is outside the allowed folders or would overwrite an existing file.
  nlohmann::json check(const std::string& tool, nlohmann::json args) const;

  static bool is_path_key(const std::string& key);

 private:
  std::vector<std::filesystem::path> roots_;
  bool overwrite_;
};

}  // namespace et::agent
