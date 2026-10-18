#include "model/timeline.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace et::model {

using std::invalid_argument;

namespace {
void check_audio(const AudioProps& a, double length) {
  if (a.volume_db < -60 || a.volume_db > 24) throw invalid_argument("volume_db must be within [-60, 24]");
  if (a.fade_in < 0 || a.fade_out < 0) throw invalid_argument("fades must be >= 0");
  if (a.fade_in + a.fade_out > length + 1e-9) throw invalid_argument("fade_in + fade_out exceed clip length");
}
json audio_json(const AudioProps& a) {
  return {{"volume_db", a.volume_db}, {"fade_in", a.fade_in}, {"fade_out", a.fade_out}, {"denoise", a.denoise}};
}
AudioProps audio_from(const json& j) {
  return {j.value("volume_db", 0.0), j.value("fade_in", 0.0), j.value("fade_out", 0.0), j.value("denoise", false)};
}
}  // namespace

const Asset& Project::add_asset(Asset a) {
  for (auto& e : assets)
    if (e.path == a.path) return e;
  a.id = "a" + std::to_string(next_asset++);
  assets.push_back(std::move(a));
  return assets.back();
}

const Asset& Project::asset(const std::string& id) const {
  for (auto& a : assets)
    if (a.id == id) return a;
  throw invalid_argument("unknown asset: " + id);
}

void Project::check_range(const Asset& a, double in, double out) const {
  if (!a.has_video) throw invalid_argument("asset " + a.id + " has no video stream");
  if (in < 0 || out <= in) throw invalid_argument("invalid range: need 0 <= in < out");
  if (out > a.duration + 1e-3)
    throw invalid_argument("out exceeds source duration (" + std::to_string(a.duration) + "s)");
}

Clip& Project::clip_ref(const std::string& id) {
  for (auto& c : clips)
    if (c.id == id) return c;
  throw invalid_argument("unknown clip: " + id);
}

const Clip& Project::add_clip(const std::string& asset_id, double in, double out, int index) {
  check_range(asset(asset_id), in, out);
  if (index < 0 || index > static_cast<int>(clips.size())) index = static_cast<int>(clips.size());
  Clip c{"c" + std::to_string(next_clip++), asset_id, in, out, {}, {}};
  return *clips.insert(clips.begin() + index, std::move(c));
}

void Project::trim_clip(const std::string& id, double in, double out) {
  Clip& c = clip_ref(id);
  check_range(asset(c.asset_id), in, out);
  check_audio(c.audio, out - in);
  c.in = in;
  c.out = out;
}

MusicClip& Project::music_ref(const std::string& id) {
  for (auto& m : music)
    if (m.id == id) return m;
  throw invalid_argument("unknown music clip: " + id);
}

void Project::check_music(const Asset& a, double in, double out, double start) const {
  if (!a.has_audio) throw invalid_argument("asset " + a.id + " has no audio stream");
  if (in < 0 || out <= in) throw invalid_argument("invalid range: need 0 <= in < out");
  if (out > a.duration + 1e-3)
    throw invalid_argument("out exceeds source duration (" + std::to_string(a.duration) + "s)");
  if (start < 0) throw invalid_argument("start must be >= 0");
}

const MusicClip& Project::add_music(const std::string& asset_id, double in, double out, double start, double volume_db) {
  check_music(asset(asset_id), in, out, start);
  MusicClip m{"m" + std::to_string(next_music), asset_id, in, out, start, {volume_db, 0, 0, false}};
  check_audio(m.audio, m.length());
  ++next_music;
  music.push_back(std::move(m));
  return music.back();
}

void Project::update_music(const std::string& id, std::optional<double> in, std::optional<double> out,
                           std::optional<double> start) {
  MusicClip m = music_ref(id);
  if (in) m.in = *in;
  if (out) m.out = *out;
  if (start) m.start = *start;
  check_music(asset(m.asset_id), m.in, m.out, m.start);
  check_audio(m.audio, m.length());
  music_ref(id) = m;
}

void Project::delete_music(const std::string& id) {
  MusicClip& m = music_ref(id);
  music.erase(music.begin() + (&m - music.data()));
}

void Project::set_audio(const std::string& id, const AudioUpdate& u) {
  AudioProps* target = nullptr;
  double length = 0;
  if (!id.empty() && id[0] == 'm') {
    auto& m = music_ref(id);
    target = &m.audio;
    length = m.length();
  } else {
    auto& c = clip_ref(id);
    target = &c.audio;
    length = c.length();
  }
  AudioProps a = *target;
  if (u.volume_db) a.volume_db = *u.volume_db;
  if (u.fade_in) a.fade_in = *u.fade_in;
  if (u.fade_out) a.fade_out = *u.fade_out;
  if (u.denoise) a.denoise = *u.denoise;
  check_audio(a, length);
  *target = a;
}

void Project::set_duck(bool enabled, const std::string& strength) {
  if (strength != "light" && strength != "medium" && strength != "strong")
    throw invalid_argument("strength must be light, medium or strong");
  duck = {enabled, strength};
}

void Project::set_loudnorm(bool enabled, double lufs, double true_peak) {
  if (lufs < -40 || lufs > -5) throw invalid_argument("lufs must be within [-40, -5]");
  if (true_peak < -9 || true_peak > 0) throw invalid_argument("true_peak must be within [-9, 0]");
  loudnorm = {enabled, lufs, true_peak};
}

void Project::move_clip(const std::string& id, int index) {
  Clip& c = clip_ref(id);
  if (index < 0 || index >= static_cast<int>(clips.size())) throw invalid_argument("index out of range");
  Clip copy = c;
  clips.erase(clips.begin() + (&c - clips.data()));
  clips.insert(clips.begin() + index, copy);
}

void Project::delete_clip(const std::string& id) {
  Clip& c = clip_ref(id);
  clips.erase(clips.begin() + (&c - clips.data()));
}

double Project::duration() const {
  double d = 0;
  for (auto& c : clips) d += c.length();
  return d;
}

namespace {
void check_cue(const SubtitleCue& c) {
  if (c.start < 0 || c.end <= c.start) throw invalid_argument("subtitle needs 0 <= start < end");
  if (c.text.find_first_not_of(" \t\r\n") == std::string::npos) throw invalid_argument("subtitle text is empty");
}
bool is_hex_color(const std::string& s) {
  if (s.size() != 7 || s[0] != '#') return false;
  return std::all_of(s.begin() + 1, s.end(), [](unsigned char ch) { return std::isxdigit(ch); });
}
void sort_cues(std::vector<SubtitleCue>& v) {
  std::stable_sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.start < b.start; });
}
}  // namespace

void Project::set_subtitles(std::vector<SubtitleCue> cues) {
  for (auto& c : cues) check_cue(c);
  sort_cues(cues);
  int n = 1;
  for (auto& c : cues) c.id = "s" + std::to_string(n++);
  subtitles = std::move(cues);
  next_cue = n;
}

void Project::update_subtitle(const std::string& id, std::optional<double> start, std::optional<double> end,
                              std::optional<std::string> text) {
  auto it = std::find_if(subtitles.begin(), subtitles.end(), [&](auto& c) { return c.id == id; });
  if (it == subtitles.end()) throw invalid_argument("unknown subtitle: " + id);
  SubtitleCue c = *it;
  if (start) c.start = *start;
  if (end) c.end = *end;
  if (text) c.text = *text;
  check_cue(c);
  *it = c;
  sort_cues(subtitles);
}

void Project::delete_subtitle(const std::string& id) {
  auto it = std::find_if(subtitles.begin(), subtitles.end(), [&](auto& c) { return c.id == id; });
  if (it == subtitles.end()) throw invalid_argument("unknown subtitle: " + id);
  subtitles.erase(it);
}

void Project::set_subtitle_style(const json& patch) {
  SubtitleStyle s = subtitle_style;
  for (auto& [k, v] : patch.items()) {
    if (k == "font") s.font = v.get<std::string>();
    else if (k == "font_size") s.font_size = v.get<double>();
    else if (k == "color") s.color = v.get<std::string>();
    else if (k == "outline_color") s.outline_color = v.get<std::string>();
    else if (k == "outline") s.outline = v.get<double>();
    else if (k == "shadow") s.shadow = v.get<double>();
    else if (k == "bold") s.bold = v.get<bool>();
    else if (k == "position") s.position = v.get<std::string>();
    else if (k == "margin_v") s.margin_v = v.get<int>();
    else if (k == "box") s.box = v.get<bool>();
    else if (k == "box_opacity") s.box_opacity = v.get<double>();
    else throw invalid_argument("unknown style field: " + k);
  }
  if (!is_hex_color(s.color) || !is_hex_color(s.outline_color)) throw invalid_argument("colors must look like #RRGGBB");
  if (s.font_size != 0 && (s.font_size < 8 || s.font_size > 400)) throw invalid_argument("font_size must be 0 (auto) or 8-400");
  if (s.outline < 0 || s.outline > 20 || s.shadow < 0 || s.shadow > 20) throw invalid_argument("outline/shadow must be within [0, 20]");
  if (s.position != "bottom" && s.position != "middle" && s.position != "top")
    throw invalid_argument("position must be bottom, middle or top");
  if (s.margin_v < 0 || s.margin_v > 2000) throw invalid_argument("margin_v must be within [0, 2000]");
  if (s.box_opacity < 0 || s.box_opacity > 1) throw invalid_argument("box_opacity must be within [0, 1]");
  if (s.font.find_first_of("\n,") != std::string::npos) throw invalid_argument("invalid font name");
  subtitle_style = s;
}

ColorGrade& Project::grade_ref(const std::string& clip_id) {
  return clip_id.empty() ? grade : clip_ref(clip_id).grade;
}

const ColorGrade& Project::grade_for(const std::string& clip_id) const {
  return const_cast<Project*>(this)->grade_ref(clip_id);
}

namespace {
void need_range(const char* name, double v, double lo, double hi) {
  if (!(v >= lo && v <= hi))
    throw invalid_argument(std::string(name) + " must be within [" + std::to_string(lo) + ", " + std::to_string(hi) + "]");
}
const char* const kCurvePresets[] = {"color_negative", "cross_process", "darker", "increase_contrast", "lighter",
                                     "linear_contrast", "medium_contrast", "negative", "strong_contrast", "vintage"};
bool has_lut_extension(const std::string& path) {
  auto dot = path.find_last_of('.');
  if (dot == std::string::npos) return false;
  std::string ext = path.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
  return ext == "cube" || ext == "3dl" || ext == "dat" || ext == "m3d" || ext == "csp";
}
}  // namespace

void Project::set_color(const std::string& clip_id, const json& patch) {
  ColorGrade g = grade_ref(clip_id);
  for (auto& [k, v] : patch.items()) {
    if (k == "clip_id") continue;
    if (k == "brightness") g.brightness = v.get<double>();
    else if (k == "contrast") g.contrast = v.get<double>();
    else if (k == "saturation") g.saturation = v.get<double>();
    else if (k == "gamma") g.gamma = v.get<double>();
    else if (k == "warmth") g.warmth = v.get<double>();
    else if (k == "tint") g.tint = v.get<double>();
    else throw invalid_argument("unknown color field: " + k);
  }
  need_range("brightness", g.brightness, -1, 1);
  need_range("contrast", g.contrast, 0, 3);
  need_range("saturation", g.saturation, 0, 3);
  need_range("gamma", g.gamma, 0.1, 5);
  need_range("warmth", g.warmth, -1, 1);
  need_range("tint", g.tint, -1, 1);
  grade_ref(clip_id) = g;
}

void Project::set_curve(const std::string& clip_id, const std::string& preset,
                        const std::vector<std::pair<double, double>>& points) {
  if (!preset.empty() && !points.empty()) throw invalid_argument("give either a curve preset or points, not both");
  if (!preset.empty() && std::find(std::begin(kCurvePresets), std::end(kCurvePresets), preset) == std::end(kCurvePresets)) {
    std::string all;
    for (auto* p : kCurvePresets) all += std::string(all.empty() ? "" : ", ") + p;
    throw invalid_argument("unknown curve preset; use one of: " + all);
  }
  if (!points.empty()) {
    if (points.size() < 2 || points.size() > 16) throw invalid_argument("a curve needs 2-16 points");
    for (size_t i = 0; i < points.size(); ++i) {
      need_range("curve x", points[i].first, 0, 1);
      need_range("curve y", points[i].second, 0, 1);
      if (i > 0 && points[i].first <= points[i - 1].first) throw invalid_argument("curve x values must be strictly increasing");
    }
  }
  ColorGrade& g = grade_ref(clip_id);
  g.curve_preset = preset;
  g.curve_points = points;
}

void Project::set_lut(const std::string& clip_id, const std::string& path, double intensity) {
  if (!has_lut_extension(path)) throw invalid_argument("LUT must be a .cube, .3dl, .dat, .m3d or .csp file");
  need_range("intensity", intensity, 0, 1);
  if (path.find('\n') != std::string::npos) throw invalid_argument("invalid LUT path");
  ColorGrade& g = grade_ref(clip_id);
  g.lut_path = path;
  g.lut_intensity = intensity;
}

void Project::reset_color(const std::string& clip_id) { grade_ref(clip_id) = {}; }

namespace {
json grade_json(const ColorGrade& g) {
  json pts = json::array();
  for (auto& [x, y] : g.curve_points) pts.push_back({x, y});
  return {{"brightness", g.brightness}, {"contrast", g.contrast}, {"saturation", g.saturation}, {"gamma", g.gamma},
          {"warmth", g.warmth}, {"tint", g.tint}, {"curve_preset", g.curve_preset}, {"curve_points", pts},
          {"lut_path", g.lut_path}, {"lut_intensity", g.lut_intensity}};
}
ColorGrade grade_from(const json& j) {
  ColorGrade g;
  g.brightness = j.value("brightness", 0.0);
  g.contrast = j.value("contrast", 1.0);
  g.saturation = j.value("saturation", 1.0);
  g.gamma = j.value("gamma", 1.0);
  g.warmth = j.value("warmth", 0.0);
  g.tint = j.value("tint", 0.0);
  g.curve_preset = j.value("curve_preset", "");
  for (auto& p : j.value("curve_points", json::array())) g.curve_points.emplace_back(p[0].get<double>(), p[1].get<double>());
  g.lut_path = j.value("lut_path", "");
  g.lut_intensity = j.value("lut_intensity", 1.0);
  return g;
}
}  // namespace

json Project::to_json() const {
  json ja = json::array(), jc = json::array(), jm = json::array(), js = json::array();
  for (auto& c : subtitles) js.push_back({{"id", c.id}, {"start", c.start}, {"end", c.end}, {"text", c.text}});
  const auto& st = subtitle_style;
  json jstyle = {{"font", st.font}, {"font_size", st.font_size}, {"color", st.color},
                 {"outline_color", st.outline_color}, {"outline", st.outline}, {"shadow", st.shadow},
                 {"bold", st.bold}, {"position", st.position}, {"margin_v", st.margin_v}, {"box", st.box},
                 {"box_opacity", st.box_opacity}};
  for (auto& a : assets)
    ja.push_back({{"id", a.id}, {"path", a.path}, {"duration", a.duration}, {"has_video", a.has_video},
                  {"has_audio", a.has_audio}, {"width", a.width}, {"height", a.height}});
  double t = 0;
  for (auto& c : clips) {
    jc.push_back({{"id", c.id}, {"asset", c.asset_id}, {"in", c.in}, {"out", c.out}, {"start", t},
                  {"audio", audio_json(c.audio)}, {"color", grade_json(c.grade)}});
    t += c.length();
  }
  for (auto& m : music)
    jm.push_back({{"id", m.id}, {"asset", m.asset_id}, {"in", m.in}, {"out", m.out}, {"start", m.start},
                  {"audio", audio_json(m.audio)}});
  return {{"name", name}, {"width", width}, {"height", height}, {"fps", fps}, {"sample_rate", sample_rate},
          {"duration", duration()}, {"assets", ja}, {"clips", jc}, {"music", jm}, {"color", grade_json(grade)}, {"subtitles", js}, {"subtitle_style", jstyle},
          {"duck", {{"enabled", duck.enabled}, {"strength", duck.strength}}},
          {"loudnorm", {{"enabled", loudnorm.enabled}, {"lufs", loudnorm.lufs}, {"true_peak", loudnorm.true_peak}}},
          {"next_asset", next_asset}, {"next_clip", next_clip}, {"next_music", next_music}, {"next_cue", next_cue}};
}

Project Project::from_json(const json& j) {
  Project p;
  p.name = j.value("name", "untitled");
  p.width = j.value("width", 1920);
  p.height = j.value("height", 1080);
  p.fps = j.value("fps", 30.0);
  p.sample_rate = j.value("sample_rate", 48000);
  p.next_asset = j.value("next_asset", 1);
  p.next_clip = j.value("next_clip", 1);
  p.next_music = j.value("next_music", 1);
  p.next_cue = j.value("next_cue", 1);
  for (auto& c : j.value("subtitles", json::array()))
    p.subtitles.push_back({c["id"], c["start"], c["end"], c["text"]});
  if (j.contains("subtitle_style")) p.set_subtitle_style(j["subtitle_style"]);
  if (j.contains("duck")) p.duck = {j["duck"].value("enabled", false), j["duck"].value("strength", "medium")};
  if (j.contains("loudnorm"))
    p.loudnorm = {j["loudnorm"].value("enabled", false), j["loudnorm"].value("lufs", -16.0),
                  j["loudnorm"].value("true_peak", -1.5)};
  for (auto& a : j.value("assets", json::array()))
    p.assets.push_back({a["id"], a["path"], a["duration"], a["has_video"], a["has_audio"], a["width"], a["height"]});
  if (j.contains("color")) p.grade = grade_from(j["color"]);
  for (auto& c : j.value("clips", json::array()))
    p.clips.push_back({c["id"], c["asset"], c["in"], c["out"], audio_from(c.value("audio", json::object())),
                       grade_from(c.value("color", json::object()))});
  for (auto& m : j.value("music", json::array()))
    p.music.push_back({m["id"], m["asset"], m["in"], m["out"], m["start"], audio_from(m.value("audio", json::object()))});
  return p;
}

}  // namespace et::model
