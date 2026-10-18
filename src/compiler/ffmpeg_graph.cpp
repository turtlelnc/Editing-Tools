#include "compiler/ffmpeg_graph.h"

#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>

namespace et::compiler {

namespace {
std::string num(double v) {
  std::ostringstream s;
  s << std::fixed << std::setprecision(6) << v;
  return s.str();
}

// Per-clip sound shaping applied after trimming: denoise, gain, fades.
std::string audio_effects(const model::AudioProps& a, double length) {
  std::string f;
  if (a.denoise) f += ",afftdn=nr=12:nf=-35";
  if (a.volume_db != 0) f += ",volume=" + num(a.volume_db) + "dB";
  if (a.fade_in > 0) f += ",afade=t=in:st=0:d=" + num(a.fade_in);
  if (a.fade_out > 0) f += ",afade=t=out:st=" + num(length - a.fade_out) + ":d=" + num(a.fade_out);
  return f;
}

// Sidechain compressor ratio for each ducking strength.
int duck_ratio(const std::string& strength) {
  if (strength == "light") return 3;
  if (strength == "strong") return 12;
  return 6;
}
}  // namespace

std::vector<std::string> compile(const model::Project& p, const Options& opt) {
  if (p.clips.empty()) throw std::invalid_argument("timeline is empty");

  std::vector<std::string> args = {"ffmpeg", "-y", "-v", "error", "-nostdin"};
  std::map<std::string, int> input_index;
  auto add_input = [&](const std::string& asset_id) {
    if (input_index.count(asset_id)) return;
    int idx = static_cast<int>(input_index.size());
    input_index[asset_id] = idx;
    args.insert(args.end(), {"-i", p.asset(asset_id).path});
  };
  for (auto& c : p.clips) add_input(c.asset_id);
  for (auto& m : p.music) add_input(m.asset_id);

  const std::string size = std::to_string(p.width) + ":" + std::to_string(p.height);
  const std::string fmt = "aresample=" + std::to_string(p.sample_rate) + ",aformat=sample_fmts=fltp:channel_layouts=stereo";
  std::ostringstream g, concat_in;

  // Main track: video + the clips' own audio, concatenated.
  for (size_t i = 0; i < p.clips.size(); ++i) {
    const auto& c = p.clips[i];
    const auto& a = p.asset(c.asset_id);
    const int k = input_index[c.asset_id];
    const std::string v = "v" + std::to_string(i), au = "a" + std::to_string(i);

    g << "[" << k << ":v]trim=start=" << num(c.in) << ":end=" << num(c.out)
      << ",setpts=PTS-STARTPTS,fps=" << num(p.fps) << ",scale=" << size
      << ":force_original_aspect_ratio=decrease,pad=" << size << ":(ow-iw)/2:(oh-ih)/2,setsar=1,format=yuv420p["
      << v << "];";

    if (a.has_audio) {
      g << "[" << k << ":a]atrim=start=" << num(c.in) << ":end=" << num(c.out) << ",asetpts=PTS-STARTPTS," << fmt
        << audio_effects(c.audio, c.length()) << "[" << au << "];";
    } else {
      g << "anullsrc=r=" << p.sample_rate << ":cl=stereo,atrim=duration=" << num(c.length())
        << ",asetpts=PTS-STARTPTS[" << au << "];";
    }
    concat_in << "[" << v << "][" << au << "]";
  }
  g << concat_in.str() << "concat=n=" << p.clips.size() << ":v=1:a=1[v][dlg]";

  // Music bus: each clip trimmed, shaped, delayed to its start, then summed.
  std::string mix = "dlg";
  if (!p.music.empty()) {
    std::string bus_in;
    for (size_t i = 0; i < p.music.size(); ++i) {
      const auto& m = p.music[i];
      const std::string label = "m" + std::to_string(i);
      g << ";[" << input_index[m.asset_id] << ":a]atrim=start=" << num(m.in) << ":end=" << num(m.out)
        << ",asetpts=PTS-STARTPTS," << fmt << audio_effects(m.audio, m.length());
      if (m.start > 0) g << ",adelay=delays=" << std::llround(m.start * 1000) << ":all=1";
      g << "[" << label << "]";
      bus_in += "[" + label + "]";
    }
    if (p.music.size() == 1) g << ";" << bus_in << "anull[mus]";
    else g << ";" << bus_in << "amix=inputs=" << p.music.size() << ":duration=longest:normalize=0[mus]";

    if (p.duck.enabled) {
      g << ";[dlg]asplit=2[dlg_mix][dlg_sc];[mus][dlg_sc]sidechaincompress=threshold=0.02:ratio="
        << duck_ratio(p.duck.strength) << ":attack=20:release=600[mus_d]";
      g << ";[dlg_mix][mus_d]amix=inputs=2:duration=first:normalize=0[mix]";
    } else {
      g << ";[dlg][mus]amix=inputs=2:duration=first:normalize=0[mix]";
    }
    mix = "mix";
  }

  if (p.loudnorm.enabled)
    g << ";[" << mix << "]loudnorm=I=" << num(p.loudnorm.lufs) << ":TP=" << num(p.loudnorm.true_peak) << ":LRA=11[a]";
  else
    g << ";[" << mix << "]anull[a]";

  args.insert(args.end(), {"-filter_complex", g.str(), "-map", "[v]", "-map", "[a]", "-c:v", "libx264"});
  if (opt.preview) args.insert(args.end(), {"-preset", "ultrafast", "-crf", "32"});
  else args.insert(args.end(), {"-preset", "medium", "-crf", "18"});
  args.insert(args.end(), {"-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart", opt.output});
  return args;
}

}  // namespace et::compiler
