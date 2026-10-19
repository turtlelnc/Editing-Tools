#pragma once
#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

namespace et::util {

// True when `s` is entirely valid UTF-8.
bool valid_utf8(const std::string& s);

// Replaces each invalid byte (or truncated sequence) with U+FFFD, so the result is always safe to put in JSON.
std::string sanitize_utf8(const std::string& s);

// Cuts `s` to at most `max_bytes` bytes without splitting a code point (for titles, summaries, logs).
std::string truncate_utf8(const std::string& s, size_t max_bytes);

// Length of the longest prefix of `s` that ends on a code-point boundary. Trailing bytes that are the beginning
// of a multi-byte code point are excluded, which is what a streaming text sink needs before showing a delta.
size_t utf8_complete_prefix(const std::string& s);

// Returns a copy of `j` with every string (keys included) passed through sanitize_utf8.
nlohmann::json sanitize_json_strings(const nlohmann::json& j);

// j.dump(indent), but repairs invalid UTF-8 instead of throwing. Model output and ffmpeg stderr are arbitrary
// bytes, and losing a whole turn because one byte is malformed is not acceptable.
std::string dump_json(const nlohmann::json& j, int indent = -1);

}  // namespace et::util
