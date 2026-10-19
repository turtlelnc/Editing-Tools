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

// Applied in this order: warmth/tint, brightness/contrast/saturation/gamma, curve, LUT.
struct ColorGrade {
  double brightness = 0;  // [-1, 1]
  double contrast = 1;    // [0, 3]
  double saturation = 1;  // [0, 3]
  double gamma = 1;       // [0.1, 5]
  double warmth = 0;      // [-1, 1]; positive = warmer (orange), negative = cooler (blue)
  double tint = 0;        // [-1, 1]; positive = magenta, negative = green
  std::string curve_preset;                           // an ffmpeg curves preset, or empty
  std::vector<std::pair<double, double>> curve_points;  // master curve (x, y) in [0, 1], or empty
  std::string lut_path;                               // .cube/.3dl/.dat/.m3d/.csp, or empty
  double lut_intensity = 1;                           // 0 = off .. 1 = full

  bool has_adjustments() const {
    return brightness != 0 || contrast != 1 || saturation != 1 || gamma != 1 || warmth != 0 || tint != 0;
  }
  bool neutral() const {
    return !has_adjustments() && curve_preset.empty() && curve_points.empty() && lut_path.empty();
  }
};

struct Clip {
  std::string id, asset_id;
  double in = 0, out = 0;  // seconds within the source
  AudioProps audio;        // applies to this clip's own sound
  ColorGrade grade;        // applied on top of the project-wide grade
  double speed = 1.0;      // playback speed (0.25-4); 2 = twice as fast, so the clip is half as long on screen
  double length() const { return (out - in) / speed; }
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

struct SubtitleCue {
  std::string id;
  double start = 0, end = 0;  // seconds on the output timeline
  std::string text;           // may contain '\n' for manual line breaks
};

struct SubtitleStyle {
  std::string font;                 // empty = platform default CJK-capable font
  double font_size = 0;             // px at project height; 0 = auto (height / 18)
  std::string color = "#FFFFFF", outline_color = "#000000";
  double outline = 2.5, shadow = 1;
  bool bold = false;
  std::string position = "bottom";  // bottom | middle | top
  int margin_v = 0;                 // px from the edge; 0 = auto
  bool box = false;                 // opaque background box behind text
  double box_opacity = 0.6;
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
  ColorGrade grade;  // project-wide look, applied to every clip before its own grade
  std::vector<SubtitleCue> subtitles;  // sorted by start
  SubtitleStyle subtitle_style;
  int next_asset = 1, next_clip = 1, next_music = 1, next_cue = 1;

  const Asset& add_asset(Asset a);  // dedupes by path
  const Asset& asset(const std::string& id) const;
  const Clip& add_clip(const std::string& asset_id, double in, double out, int index = -1);
  void set_speed(const std::string& clip_id, double speed);
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

  // Color. clip_id "" targets the project-wide grade. Each call validates fully before changing anything.
  void set_color(const std::string& clip_id, const json& patch);  // brightness contrast saturation gamma warmth tint
  void set_curve(const std::string& clip_id, const std::string& preset,
                 const std::vector<std::pair<double, double>>& points);  // both empty clears the curve
  void set_lut(const std::string& clip_id, const std::string& path, double intensity);
  void reset_color(const std::string& clip_id);
  const ColorGrade& grade_for(const std::string& clip_id) const;

  // Replaces all subtitles (ids are reassigned). Cue ids in the input are ignored.
  void set_subtitles(std::vector<SubtitleCue> cues);
  void update_subtitle(const std::string& id, std::optional<double> start, std::optional<double> end,
                       std::optional<std::string> text);
  void delete_subtitle(const std::string& id);
  // Merges the given fields (font, font_size, color, outline_color, outline, shadow, bold, position,
  // margin_v, box, box_opacity) into the style.
  void set_subtitle_style(const json& patch);
  double duration() const;

  json to_json() const;
  static Project from_json(const json& j);

 private:
  Clip& clip_ref(const std::string& id);
  MusicClip& music_ref(const std::string& id);
  ColorGrade& grade_ref(const std::string& clip_id);
  void check_music(const Asset& a, double in, double out, double start) const;
  void check_range(const Asset& a, double in, double out) const;
};

}  // namespace et::model
