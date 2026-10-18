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

inline std::string unbase64(const std::string& in) {
  auto val = [](unsigned char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::string out;
  unsigned acc = 0;
  int bits = 0;
  for (unsigned char c : in) {
    int v = val(c);
    if (v < 0) continue;  // padding and whitespace
    acc = (acc << 6) | static_cast<unsigned>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((acc >> bits) & 0xFF);
    }
  }
  return out;
}

}  // namespace et::util
