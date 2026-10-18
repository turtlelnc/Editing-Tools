#include "model/timeline.h"

#include <algorithm>
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
  Clip c{"c" + std::to_string(next_clip++), asset_id, in, out, {}};
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

json Project::to_json() const {
  json ja = json::array(), jc = json::array(), jm = json::array();
  for (auto& a : assets)
    ja.push_back({{"id", a.id}, {"path", a.path}, {"duration", a.duration}, {"has_video", a.has_video},
                  {"has_audio", a.has_audio}, {"width", a.width}, {"height", a.height}});
  double t = 0;
  for (auto& c : clips) {
    jc.push_back({{"id", c.id}, {"asset", c.asset_id}, {"in", c.in}, {"out", c.out}, {"start", t},
                  {"audio", audio_json(c.audio)}});
    t += c.length();
  }
  for (auto& m : music)
    jm.push_back({{"id", m.id}, {"asset", m.asset_id}, {"in", m.in}, {"out", m.out}, {"start", m.start},
                  {"audio", audio_json(m.audio)}});
  return {{"name", name}, {"width", width}, {"height", height}, {"fps", fps}, {"sample_rate", sample_rate},
          {"duration", duration()}, {"assets", ja}, {"clips", jc}, {"music", jm},
          {"duck", {{"enabled", duck.enabled}, {"strength", duck.strength}}},
          {"loudnorm", {{"enabled", loudnorm.enabled}, {"lufs", loudnorm.lufs}, {"true_peak", loudnorm.true_peak}}},
          {"next_asset", next_asset}, {"next_clip", next_clip}, {"next_music", next_music}};
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
  if (j.contains("duck")) p.duck = {j["duck"].value("enabled", false), j["duck"].value("strength", "medium")};
  if (j.contains("loudnorm"))
    p.loudnorm = {j["loudnorm"].value("enabled", false), j["loudnorm"].value("lufs", -16.0),
                  j["loudnorm"].value("true_peak", -1.5)};
  for (auto& a : j.value("assets", json::array()))
    p.assets.push_back({a["id"], a["path"], a["duration"], a["has_video"], a["has_audio"], a["width"], a["height"]});
  for (auto& c : j.value("clips", json::array()))
    p.clips.push_back({c["id"], c["asset"], c["in"], c["out"], audio_from(c.value("audio", json::object()))});
  for (auto& m : j.value("music", json::array()))
    p.music.push_back({m["id"], m["asset"], m["in"], m["out"], m["start"], audio_from(m.value("audio", json::object()))});
  return p;
}

}  // namespace et::model
