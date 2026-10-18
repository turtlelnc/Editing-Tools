#pragma once
#include <optional>
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

struct AudioProps {
  double volume_db = 0, fade_in = 0, fade_out = 0;  // fades in seconds
  bool denoise = false;
};

struct Clip {
  std::string id, asset_id;
  double in = 0, out = 0;  // seconds within the source
  AudioProps audio;        // applies to this clip's own sound
  double length() const { return out - in; }
};

// Background music placed on the timeline at `start`; plays source [in, out).
struct MusicClip {
  std::string id, asset_id;
  double in = 0, out = 0, start = 0;
  AudioProps audio;
  double length() const { return out - in; }
};

struct Duck {  // lower all music while the main clips' audio is present
  bool enabled = false;
  std::string strength = "medium";  // light | medium | strong
};

struct LoudNorm {  // EBU R128 normalization of the final mix
  bool enabled = false;
  double lufs = -16, true_peak = -1.5;
};

struct AudioUpdate {  // unset fields are left unchanged
  std::optional<double> volume_db, fade_in, fade_out;
  std::optional<bool> denoise;
};

// Throws std::invalid_argument on any invalid edit; the project is left unchanged.
struct Project {
  std::string name = "untitled";
  int width = 1920, height = 1080, sample_rate = 48000;
  double fps = 30;
  std::vector<Asset> assets;
  std::vector<Clip> clips;  // main video track, played back to back
  std::vector<MusicClip> music;
  Duck duck;
  LoudNorm loudnorm;
  int next_asset = 1, next_clip = 1, next_music = 1;

  const Asset& add_asset(Asset a);  // dedupes by path
  const Asset& asset(const std::string& id) const;
  const Clip& add_clip(const std::string& asset_id, double in, double out, int index = -1);
  void trim_clip(const std::string& id, double in, double out);
  void move_clip(const std::string& id, int index);
  void delete_clip(const std::string& id);

  const MusicClip& add_music(const std::string& asset_id, double in, double out, double start, double volume_db = 0);
  void update_music(const std::string& id, std::optional<double> in, std::optional<double> out,
                    std::optional<double> start);
  void delete_music(const std::string& id);
  void set_audio(const std::string& id, const AudioUpdate& u);  // id of a clip ("c*") or music clip ("m*")
  void set_duck(bool enabled, const std::string& strength);
  void set_loudnorm(bool enabled, double lufs, double true_peak);
  double duration() const;

  json to_json() const;
  static Project from_json(const json& j);

 private:
  Clip& clip_ref(const std::string& id);
  MusicClip& music_ref(const std::string& id);
  void check_music(const Asset& a, double in, double out, double start) const;
  void check_range(const Asset& a, double in, double out) const;
};

}  // namespace et::model
