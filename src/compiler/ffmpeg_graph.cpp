#include "compiler/ffmpeg_graph.h"

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
}  // namespace

std::vector<std::string> compile(const model::Project& p, const Options& opt) {
  if (p.clips.empty()) throw std::invalid_argument("timeline is empty");

  std::vector<std::string> args = {"ffmpeg", "-y", "-v", "error", "-nostdin"};
  std::map<std::string, int> input_index;
  for (auto& c : p.clips) {
    if (input_index.count(c.asset_id)) continue;
    input_index[c.asset_id] = static_cast<int>(input_index.size());
    args.insert(args.end(), {"-i", p.asset(c.asset_id).path});
  }

  const std::string size = std::to_string(p.width) + ":" + std::to_string(p.height);
  std::ostringstream g;
  std::ostringstream concat_in;
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
      g << "[" << k << ":a]atrim=start=" << num(c.in) << ":end=" << num(c.out) << ",asetpts=PTS-STARTPTS,aresample="
        << p.sample_rate << ",aformat=sample_fmts=fltp:channel_layouts=stereo[" << au << "];";
    } else {
      g << "anullsrc=r=" << p.sample_rate << ":cl=stereo,atrim=duration=" << num(c.length())
        << ",asetpts=PTS-STARTPTS[" << au << "];";
    }
    concat_in << "[" << v << "][" << au << "]";
  }
  g << concat_in.str() << "concat=n=" << p.clips.size() << ":v=1:a=1[v][a]";

  args.insert(args.end(), {"-filter_complex", g.str(), "-map", "[v]", "-map", "[a]", "-c:v", "libx264"});
  if (opt.preview) args.insert(args.end(), {"-preset", "ultrafast", "-crf", "32"});
  else args.insert(args.end(), {"-preset", "medium", "-crf", "18"});
  args.insert(args.end(), {"-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart", opt.output});
  return args;
}

}  // namespace et::compiler
