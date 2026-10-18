#pragma once
#include "mcp/server.h"

namespace et::tools {

// Runs ffprobe and returns a trimmed summary: duration, format, and per-stream info.
mcp::json probe_media(const std::string& path);

void register_media_tools(mcp::Server& server);

}  // namespace et::tools
