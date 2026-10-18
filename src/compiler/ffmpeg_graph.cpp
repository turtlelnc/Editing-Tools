#include "compiler/ffmpeg_graph.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "process.h"

namespace et::compiler {

bool is_filter_safe_path(const std::string& path) {
  return !path.empty() &&
         path.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_./-") == std::string::npos;
}

namespace {

std::string num(double v) {
  std::ostringstream s;
  s << std::fixed << std::setprecision(6) << v;
  return s.str();
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) out += (i ? sep : "") + v[i];
  return out;
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

std::string lut_file(const Options& opt, const std::string& path) {
  auto it = opt.lut_paths.find(path);
  const std::string& f = it != opt.lut_paths.end() ? it->second : path;
  if (!is_filter_safe_path(f)) throw std::invalid_argument("LUT path needs a filtergraph-safe alias: " + path);
  return f;
}

// Appends statements applying `cg` to stream `cur`; returns the label of the graded stream.
std::string grade_stage(std::vector<std::string>& st, std::string cur, const model::ColorGrade& cg, const Options& opt,
                        const std::string& uid) {
  if (cg.neutral()) return cur;

  std::vector<std::string> chain;
  if (cg.warmth != 0) {
    // The filter takes the light's colour temperature: lower = warmer picture. Its response is far weaker on the
    // cool side, so that side spans a much wider range to make +1 and -1 look about equally strong.
    const double kelvin = cg.warmth > 0 ? 6500 - 3000 * cg.warmth : 6500 - 13500 * cg.warmth;
    chain.push_back("colortemperature=temperature=" + num(kelvin));
  }
  if (cg.tint != 0) chain.push_back("colorchannelmixer=gg=" + num(1 - 0.2 * cg.tint));
  if (cg.brightness != 0 || cg.contrast != 1 || cg.saturation != 1 || cg.gamma != 1)
    chain.push_back("eq=brightness=" + num(cg.brightness) + ":contrast=" + num(cg.contrast) +
                    ":saturation=" + num(cg.saturation) + ":gamma=" + num(cg.gamma));
  if (!cg.curve_preset.empty()) chain.push_back("curves=preset=" + cg.curve_preset);
  if (!cg.curve_points.empty()) {
    std::vector<std::string> pts;
    for (auto& [x, y] : cg.curve_points) pts.push_back(num(x) + "/" + num(y));
    chain.push_back("curves=master='" + join(pts, " ") + "'");
  }
  auto flush = [&](const std::string& tag) {
    if (chain.empty()) return;
    const std::string out = uid + tag;
    st.push_back("[" + cur + "]" + join(chain, ",") + "[" + out + "]");
    cur = out;
    chain.clear();
  };
  flush("p");

  if (!cg.lut_path.empty() && cg.lut_intensity > 0) {
    const std::string lut = "lut3d=file='" + lut_file(opt, cg.lut_path) + "'";
    if (cg.lut_intensity >= 0.999) {
      chain.push_back(lut);
      flush("l");
    } else {  // blend the LUT result with the untouched picture
      const std::string k = num(cg.lut_intensity);
      st.push_back("[" + cur + "]split[" + uid + "a][" + uid + "b]");
      st.push_back("[" + uid + "b]" + lut + "[" + uid + "c]");
      st.push_back("[" + uid + "a][" + uid + "c]blend=all_expr='A*(1-" + k + ")+B*" + k + "'[" + uid + "d]");
      cur = uid + "d";
    }
  }
  return cur;
}

// A clip as it will be read: which source range, and where to seek the input.
struct Seg {
  const model::Clip* clip;
  double seek = 0;       // input-side -ss (frame mode only)
  double trim_in, trim_out;
  double length() const { return trim_out - trim_in; }
};

}  // namespace

std::vector<std::string> compile(const model::Project& p, const Options& opt) {
  if (p.clips.empty()) throw std::invalid_argument("timeline is empty");
  const bool frame = opt.frame_time >= 0;

  // Decide which clips to read.
  std::vector<Seg> segs;
  double frame_offset = 0;  // timeline time of stream time 0 (frame mode)
  double frame_local = 0;   // stream time of the wanted frame (frame mode)
  if (frame) {
    double start = 0;
    for (auto& c : p.clips) {
      if (opt.frame_time < start + c.length() - 1e-9) {
        const double tl = opt.frame_time - start;
        const double seek = c.in + std::max(0.0, tl - 3.0);  // decode only a few seconds before the frame
        segs.push_back({&c, seek, 0, c.out - seek});
        frame_offset = start + (seek - c.in);
        frame_local = tl - (seek - c.in);
        break;
      }
      start += c.length();
    }
    if (segs.empty()) throw std::invalid_argument("time is beyond the end of the timeline");
  } else {
    for (auto& c : p.clips) segs.push_back({&c, 0, c.in, c.out});
  }

  std::vector<std::string> args = {ffmpeg_bin(), "-y", "-v", "error", "-nostdin"};
  std::map<std::string, int> input_index;
  auto add_input = [&](const std::string& asset_id, double seek) {
    if (input_index.count(asset_id)) return;
    int idx = static_cast<int>(input_index.size());
    input_index[asset_id] = idx;
    if (seek > 0) args.insert(args.end(), {"-ss", num(seek)});
    args.insert(args.end(), {"-i", p.asset(asset_id).path});
  };
  for (auto& s : segs) add_input(s.clip->asset_id, s.seek);
  if (!frame)
    for (auto& m : p.music) add_input(m.asset_id, 0);

  const std::string size = std::to_string(p.width) + ":" + std::to_string(p.height);
  const std::string fmt = "aresample=" + std::to_string(p.sample_rate) + ",aformat=sample_fmts=fltp:channel_layouts=stereo";
  std::vector<std::string> st;
  std::string concat_in;

  // Main track: video (graded, then fitted to the project size) + the clips' own audio.
  for (size_t i = 0; i < segs.size(); ++i) {
    const auto& s = segs[i];
    const auto& c = *s.clip;
    const auto& a = p.asset(c.asset_id);
    const int k = input_index[c.asset_id];
    const std::string id = std::to_string(i), pre = "pre" + id, v = "v" + id, au = "a" + id;

    st.push_back("[" + std::to_string(k) + ":v]trim=start=" + num(s.trim_in) + ":end=" + num(s.trim_out) +
                 ",setpts=PTS-STARTPTS,fps=" + num(p.fps) + "[" + pre + "]");
    std::string cur = grade_stage(st, pre, p.grade, opt, "gp" + id);
    cur = grade_stage(st, cur, c.grade, opt, "gc" + id);
    st.push_back("[" + cur + "]scale=" + size + ":force_original_aspect_ratio=decrease,pad=" + size +
                 ":(ow-iw)/2:(oh-ih)/2,setsar=1,format=yuv420p[" + v + "]");
    concat_in += "[" + v + "]";

    if (!frame) {
      if (a.has_audio) {
        st.push_back("[" + std::to_string(k) + ":a]atrim=start=" + num(s.trim_in) + ":end=" + num(s.trim_out) +
                     ",asetpts=PTS-STARTPTS," + fmt + audio_effects(c.audio, c.length()) + "[" + au + "]");
      } else {
        st.push_back("anullsrc=r=" + std::to_string(p.sample_rate) + ":cl=stereo,atrim=duration=" + num(c.length()) +
                     ",asetpts=PTS-STARTPTS[" + au + "]");
      }
      concat_in += "[" + au + "]";
    }
  }
  if (frame) st.push_back(concat_in + "concat=n=" + std::to_string(segs.size()) + ":v=1:a=0[v]");
  else st.push_back(concat_in + "concat=n=" + std::to_string(segs.size()) + ":v=1:a=1[v][dlg]");

  std::string video_label = "v";
  if (!p.subtitles.empty()) {
    if (opt.ass_path.empty()) throw std::invalid_argument("project has subtitles but no ASS file path was given");
    if (!is_filter_safe_path(opt.ass_path)) throw std::invalid_argument("unsupported characters in ASS path");
    // Cue times are timeline times; in frame mode the stream starts mid-timeline, so shift in and back out.
    const std::string shift_in = frame ? "setpts=PTS+" + num(frame_offset) + "/TB," : "";
    const std::string shift_out = frame ? ",setpts=PTS-" + num(frame_offset) + "/TB" : "";
    st.push_back("[v]" + shift_in + "ass=filename='" + opt.ass_path + "'" + shift_out + "[vout]");
    video_label = "vout";
  }
  if (frame && opt.frame_max_width > 0) {
    st.push_back("[" + video_label + "]scale='min(" + std::to_string(opt.frame_max_width) + ",iw)':-2[vfit]");
    video_label = "vfit";
  }

  if (frame) {
    args.insert(args.end(), {"-filter_complex", join(st, ";"), "-map", "[" + video_label + "]", "-ss", num(frame_local),
                             "-frames:v", "1", "-an", "-c:v", "mjpeg", "-pix_fmt", "yuvj420p", "-q:v", "3", "-f", "image2",
                             "-update", "1", opt.output});
    return args;
  }

  // Music bus: each clip trimmed, shaped, delayed to its start, then summed.
  std::string mix = "dlg";
  if (!p.music.empty()) {
    std::string bus_in;
    for (size_t i = 0; i < p.music.size(); ++i) {
      const auto& m = p.music[i];
      const std::string label = "m" + std::to_string(i);
      std::string stmt = "[" + std::to_string(input_index[m.asset_id]) + ":a]atrim=start=" + num(m.in) + ":end=" + num(m.out) +
                         ",asetpts=PTS-STARTPTS," + fmt + audio_effects(m.audio, m.length());
      if (m.start > 0) stmt += ",adelay=delays=" + std::to_string(std::llround(m.start * 1000)) + ":all=1";
      st.push_back(stmt + "[" + label + "]");
      bus_in += "[" + label + "]";
    }
    if (p.music.size() == 1) st.push_back(bus_in + "anull[mus]");
    else st.push_back(bus_in + "amix=inputs=" + std::to_string(p.music.size()) + ":duration=longest:normalize=0[mus]");

    if (p.duck.enabled) {
      st.push_back("[dlg]asplit=2[dlg_mix][dlg_sc]");
      st.push_back("[mus][dlg_sc]sidechaincompress=threshold=0.02:ratio=" + std::to_string(duck_ratio(p.duck.strength)) +
                   ":attack=20:release=600[mus_d]");
      st.push_back("[dlg_mix][mus_d]amix=inputs=2:duration=first:normalize=0[mix]");
    } else {
      st.push_back("[dlg][mus]amix=inputs=2:duration=first:normalize=0[mix]");
    }
    mix = "mix";
  }

  if (p.loudnorm.enabled)
    st.push_back("[" + mix + "]loudnorm=I=" + num(p.loudnorm.lufs) + ":TP=" + num(p.loudnorm.true_peak) + ":LRA=11[a]");
  else
    st.push_back("[" + mix + "]anull[a]");

  args.insert(args.end(), {"-filter_complex", join(st, ";"), "-map", "[" + video_label + "]", "-map", "[a]", "-c:v", "libx264"});
  if (opt.preview) args.insert(args.end(), {"-preset", "ultrafast", "-crf", "32"});
  else args.insert(args.end(), {"-preset", "medium", "-crf", "18"});
  args.insert(args.end(), {"-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart", opt.output});
  return args;
}

}  // namespace et::compiler
