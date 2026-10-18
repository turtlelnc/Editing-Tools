#include "agent/policy.h"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace et::agent {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

fs::path canonical_or_weak(const fs::path& p) {
  std::error_code ec;
  fs::path c = fs::weakly_canonical(p, ec);
  return ec ? p.lexically_normal() : c;
}

// True if `inner` is `outer` or lies beneath it, compared component by component.
bool is_within(const fs::path& outer, const fs::path& inner) {
  auto o = outer.begin(), i = inner.begin();
  for (; o != outer.end(); ++o, ++i) {
    if (i == inner.end() || *o != *i) return false;
  }
  return true;
}

// Tools whose output file must not silently replace an existing one.
bool writes_new_file(const std::string& tool, const std::string& key) {
  static const std::set<std::pair<std::string, std::string>> k = {
      {"render_export", "output_path"}, {"render_preview", "output_path"}, {"export_srt", "output_path"},
      {"create_project", "project_path"}};
  return k.count({tool, key}) > 0;
}

}  // namespace

Policy::Policy(PolicyConfig cfg) : overwrite_(cfg.overwrite) {
  if (cfg.roots.empty()) throw std::invalid_argument("policy needs at least one folder");
  for (auto& r : cfg.roots) roots_.push_back(canonical_or_weak(fs::absolute(r)));
}

bool Policy::is_path_key(const std::string& key) { return key == "path" || key == "output_path" || key == "project_path"; }

fs::path Policy::resolve(const std::string& path) const {
  fs::path p(path);
  if (p.is_relative()) p = workspace() / p;
  return canonical_or_weak(p);
}

bool Policy::allowed(const fs::path& absolute_path) const {
  const fs::path p = canonical_or_weak(absolute_path);
  return std::any_of(roots_.begin(), roots_.end(), [&](const fs::path& r) { return is_within(r, p); });
}

json Policy::check(const std::string& tool, json args) const {
  if (!args.is_object()) return args;
  for (auto& [key, value] : args.items()) {
    if (!is_path_key(key) || !value.is_string()) continue;
    const std::string original = value.get<std::string>();
    const fs::path p = resolve(original);
    if (!allowed(p)) {
      std::string list;
      for (auto& r : roots_) list += (list.empty() ? "" : ", ") + r.string();
      throw std::invalid_argument("'" + original + "' is outside the folders you may use (" + list +
                                  "). Use a path inside them, or ask the user to allow that folder.");
    }
    std::error_code ec;
    if (writes_new_file(tool, key) && !overwrite_ && fs::exists(p, ec))
      throw std::invalid_argument("'" + original + "' already exists and would be overwritten. Choose a different file name.");
    value = p.string();
  }
  return args;
}

}  // namespace et::agent
