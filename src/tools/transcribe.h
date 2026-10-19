#pragma once
#include "mcp/server.h"
#include "tools/session.h"

namespace et::tools {

// Speech recognition through an external whisper.cpp CLI (no model is bundled and none is linked in).
// Configure with:
//   EDITING_TOOLS_WHISPER        path to whisper-cli (default: whisper-cli from PATH)
//   EDITING_TOOLS_WHISPER_MODEL  path to a ggml-*.bin model file (required)
void register_transcribe_tools(mcp::Server& server, std::shared_ptr<Session> session);

}  // namespace et::tools
