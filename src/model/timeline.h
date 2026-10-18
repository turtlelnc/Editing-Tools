#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace et::model {

using json = nlohmann::json;

struct Asset {
  std::string id, path;
  double duration = 0;
  bool has_video = false, has_audio = false;
  int width = 0, height = 0;
};

struct Clip {
  std::string id, asset_id;
  double in = 0, out = 0;  // seconds within the source
  double length() const { return out - in; }
};

// Throws std::invalid_argument on any invalid edit; the project is left unchanged.
struct Project {
  std::string name = "untitled";
  int width = 1920, height = 1080, sample_rate = 48000;
  double fps = 30;
  std::vector<Asset> assets;
  std::vector<Clip> clips;  // main video track, played back to back
  int next_asset = 1, next_clip = 1;

  const Asset& add_asset(Asset a);  // dedupes by path
  const Asset& asset(const std::string& id) const;
  const Clip& add_clip(const std::string& asset_id, double in, double out, int index = -1);
  void trim_clip(const std::string& id, double in, double out);
  void move_clip(const std::string& id, int index);
  void delete_clip(const std::string& id);
  double duration() const;

  json to_json() const;
  static Project from_json(const json& j);

 private:
  Clip& clip_ref(const std::string& id);
  void check_range(const Asset& a, double in, double out) const;
};

}  // namespace et::model
