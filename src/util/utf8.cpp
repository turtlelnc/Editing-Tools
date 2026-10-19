#include "util/utf8.h"

namespace et::util {

namespace {

// Decodes one code point starting at `i`. Returns its length, or 0 when the bytes are not a valid sequence.
size_t decode(const std::string& s, size_t i, unsigned& cp) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  if (c < 0x80) {
    cp = c;
    return 1;
  }
  size_t len;
  unsigned min;
  if ((c & 0xE0) == 0xC0) {
    len = 2;
    min = 0x80;
    cp = c & 0x1F;
  } else if ((c & 0xF0) == 0xE0) {
    len = 3;
    min = 0x800;
    cp = c & 0x0F;
  } else if ((c & 0xF8) == 0xF0) {
    len = 4;
    min = 0x10000;
    cp = c & 0x07;
  } else {
    return 0;
  }
  if (i + len > s.size()) return 0;
  for (size_t k = 1; k < len; ++k) {
    const unsigned char cc = static_cast<unsigned char>(s[i + k]);
    if ((cc & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (cc & 0x3F);
  }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;  // overlong, out of range, surrogate
  return len;
}

}  // namespace

bool valid_utf8(const std::string& s) { return sanitize_utf8(s) == s; }

std::string sanitize_utf8(const std::string& s) {
  static const char* kReplacement = "\xEF\xBF\xBD";
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    unsigned cp = 0;
    const size_t len = decode(s, i, cp);
    if (len == 0) {
      out += kReplacement;
      ++i;
    } else {
      out.append(s, i, len);
      i += len;
    }
  }
  return out;
}

std::string truncate_utf8(const std::string& s, size_t max_bytes) {
  if (s.size() <= max_bytes) return s;
  size_t cut = max_bytes;
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
  return s.substr(0, cut);
}

size_t utf8_complete_prefix(const std::string& s) {
  // Count trailing continuation bytes; an incomplete code point is a lead byte followed by fewer continuation
  // bytes than it needs.
  size_t cont = 0;
  while (cont < 3 && cont < s.size() &&
         (static_cast<unsigned char>(s[s.size() - 1 - cont]) & 0xC0) == 0x80)
    ++cont;
  if (cont == 0) return s.size();
  const size_t lead = s.size() - 1 - cont;
  if (lead == 0) return 0;  // nothing but continuation bytes
  const unsigned char c = static_cast<unsigned char>(s[lead]);
  size_t need = 0;
  if ((c & 0xE0) == 0xC0) need = 2;
  else if ((c & 0xF0) == 0xE0) need = 3;
  else if ((c & 0xF8) == 0xF0) need = 4;
  if (need == 0) return lead;  // an invalid lead byte: drop it and the continuation run
  return need > cont + 1 ? lead : s.size();
}

nlohmann::json sanitize_json_strings(const nlohmann::json& j) {
  if (j.is_string()) return sanitize_utf8(j.get<std::string>());
  if (j.is_object()) {
    nlohmann::json out = nlohmann::json::object();
    for (auto it = j.begin(); it != j.end(); ++it) out[sanitize_utf8(it.key())] = sanitize_json_strings(it.value());
    return out;
  }
  if (j.is_array()) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& item : j) out.push_back(sanitize_json_strings(item));
    return out;
  }
  return j;
}

std::string dump_json(const nlohmann::json& j, int indent) {
  try {
    return j.dump(indent);
  } catch (const nlohmann::json::type_error&) {
    return sanitize_json_strings(j).dump(indent);
  }
}

}  // namespace et::util
