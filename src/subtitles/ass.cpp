#include "subtitles/ass.h"

#include <cmath>
#include <cstdio>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace et::subtitles {

namespace {

// "#RRGGBB" + opacity -> ASS "&HAABBGGRR" (alpha 00 = opaque).
std::string ass_color(const std::string& hex, double opacity = 1.0) {
  int alpha = static_cast<int>(std::lround((1.0 - opacity) * 255));
  char buf[16];
  std::snprintf(buf, sizeof buf, "&H%02X%c%c%c%c%c%c", alpha, hex[5], hex[6], hex[3], hex[4], hex[1], hex[2]);
  return buf;
}

std::string ass_time(double t) {
  long cs = std::lround(t * 100);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%ld:%02ld:%02ld.%02ld", cs / 360000, cs / 6000 % 60, cs / 100 % 60, cs % 100);
  return buf;
}

std::string ass_text(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '\r') continue;
    if (c == '\n') out += "\\N";
    else if (c == '{') out += "\\{";
    else if (c == '}') out += "\\}";
    else out += c;
  }
  return out;
}

std::string default_font() {
#ifdef __APPLE__
  return "Hiragino Sans GB";  // libass can read this; PingFang lives in a private framework it cannot open
#else
  return "Noto Sans CJK SC";
#endif
}

std::string fmt_num(double v) {
  std::ostringstream s;
  s << v;
  return s.str();
}

std::string srt_time(double t) {
  long ms = std::lround(t * 1000);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%02ld:%02ld:%02ld,%03ld", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000);
  return buf;
}

}  // namespace

std::string to_ass(const model::Project& p) {
  const auto& st = p.subtitle_style;
  const double size = st.font_size > 0 ? st.font_size : p.height / 18.0;
  const int alignment = st.position == "top" ? 8 : st.position == "middle" ? 5 : 2;
  const int margin_v = st.margin_v > 0 ? st.margin_v : static_cast<int>(p.height * 0.06);
  const int margin_h = static_cast<int>(p.width * 0.06);

  // With a box, libass paints BorderStyle 3 using the outline colour, with `outline` as padding.
  const int border_style = st.box ? 3 : 1;
  const double outline = st.box ? size * 0.2 : st.outline;
  const double shadow = st.box ? 0 : st.shadow;
  const std::string outline_col = st.box ? ass_color(st.outline_color, st.box_opacity) : ass_color(st.outline_color);

  std::ostringstream o;
  o << "[Script Info]\nScriptType: v4.00+\nPlayResX: " << p.width << "\nPlayResY: " << p.height
    << "\nWrapStyle: 0\nScaledBorderAndShadow: yes\n\n[V4+ Styles]\n"
    << "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, "
       "Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, "
       "MarginR, MarginV, Encoding\n"
    << "Style: Default," << (st.font.empty() ? default_font() : st.font) << "," << fmt_num(size) << ","
    << ass_color(st.color) << "," << ass_color(st.color) << "," << outline_col << ",&H80000000,"
    << (st.bold ? -1 : 0) << ",0,0,0,100,100,0,0," << border_style << "," << fmt_num(outline) << "," << fmt_num(shadow)
    << "," << alignment << "," << margin_h << "," << margin_h << "," << margin_v << ",1\n\n"
    << "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
  for (auto& c : p.subtitles)
    o << "Dialogue: 0," << ass_time(c.start) << "," << ass_time(c.end) << ",Default,,0,0,0,," << ass_text(c.text) << "\n";
  return o.str();
}

std::vector<model::SubtitleCue> parse_srt(const std::string& raw) {
  static const std::regex time_re(
      R"((\d+):(\d{2}):(\d{2})[,.](\d{1,3})\s*-->\s*(\d+):(\d{2}):(\d{2})[,.](\d{1,3}))");
  auto to_sec = [](const std::smatch& m, int i) {
    std::string ms = m[i + 3].str();
    while (ms.size() < 3) ms += '0';
    return std::stoi(m[i]) * 3600.0 + std::stoi(m[i + 1]) * 60.0 + std::stoi(m[i + 2]) + std::stoi(ms) / 1000.0;
  };

  std::string text = raw;
  if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);  // UTF-8 BOM
  std::vector<model::SubtitleCue> cues;
  std::istringstream in(text);
  std::string line;
  model::SubtitleCue cur;
  bool open = false;
  auto flush = [&] {
    if (open && !cur.text.empty()) cues.push_back(cur);
    open = false;
    cur = {};
  };
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::smatch m;
    if (std::regex_search(line, m, time_re)) {
      flush();
      cur.start = to_sec(m, 1);
      cur.end = to_sec(m, 5);
      open = true;
    } else if (open) {
      if (line.empty()) flush();
      else cur.text += (cur.text.empty() ? "" : "\n") + line;
    }
  }
  flush();
  if (cues.empty()) throw std::invalid_argument("no subtitle cues found in SRT");
  return cues;
}

std::string to_srt(const std::vector<model::SubtitleCue>& cues) {
  std::ostringstream o;
  int n = 1;
  for (auto& c : cues) o << n++ << "\n" << srt_time(c.start) << " --> " << srt_time(c.end) << "\n" << c.text << "\n\n";
  return o.str();
}

}  // namespace et::subtitles
