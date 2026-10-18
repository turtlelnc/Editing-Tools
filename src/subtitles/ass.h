#pragma once
#include <string>

#include "model/timeline.h"

namespace et::subtitles {

// Renders the project's subtitles + style as an ASS script sized to the project resolution.
std::string to_ass(const model::Project& p);

// SRT interchange. parse_srt throws std::invalid_argument if no valid cue is found.
std::vector<model::SubtitleCue> parse_srt(const std::string& text);
std::string to_srt(const std::vector<model::SubtitleCue>& cues);

}  // namespace et::subtitles
