#pragma once
#include <string>

namespace et::util {

inline std::string base64(const std::string& in) {
  static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) |
                 static_cast<unsigned char>(in[i + 2]);
    out += {T[v >> 18], T[(v >> 12) & 63], T[(v >> 6) & 63], T[v & 63]};
  }
  if (i + 1 == in.size()) {
    unsigned v = static_cast<unsigned char>(in[i]) << 16;
    out += {T[v >> 18], T[(v >> 12) & 63], '=', '='};
  } else if (i + 2 == in.size()) {
    unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8);
    out += {T[v >> 18], T[(v >> 12) & 63], T[(v >> 6) & 63], '='};
  }
  return out;
}

}  // namespace et::util
