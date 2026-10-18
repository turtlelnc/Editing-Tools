#include "model/timeline.h"

#include <algorithm>
#include <stdexcept>

namespace et::model {

using std::invalid_argument;

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
  Clip c{"c" + std::to_string(next_clip++), asset_id, in, out};
  return *clips.insert(clips.begin() + index, std::move(c));
}

void Project::trim_clip(const std::string& id, double in, double out) {
  Clip& c = clip_ref(id);
  check_range(asset(c.asset_id), in, out);
  c.in = in;
  c.out = out;
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
  json ja = json::array(), jc = json::array();
  for (auto& a : assets)
    ja.push_back({{"id", a.id}, {"path", a.path}, {"duration", a.duration}, {"has_video", a.has_video},
                  {"has_audio", a.has_audio}, {"width", a.width}, {"height", a.height}});
  double t = 0;
  for (auto& c : clips) {
    jc.push_back({{"id", c.id}, {"asset", c.asset_id}, {"in", c.in}, {"out", c.out}, {"start", t}});
    t += c.length();
  }
  return {{"name", name}, {"width", width}, {"height", height}, {"fps", fps}, {"sample_rate", sample_rate},
          {"duration", duration()}, {"assets", ja}, {"clips", jc},
          {"next_asset", next_asset}, {"next_clip", next_clip}};
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
  for (auto& a : j.value("assets", json::array()))
    p.assets.push_back({a["id"], a["path"], a["duration"], a["has_video"], a["has_audio"], a["width"], a["height"]});
  for (auto& c : j.value("clips", json::array())) p.clips.push_back({c["id"], c["asset"], c["in"], c["out"]});
  return p;
}

}  // namespace et::model
