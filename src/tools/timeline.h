#pragma once
#include "mcp/server.h"

namespace et::tools {

// Registers project/timeline/render tools. The project lives in memory and is
// saved to its project.json after every mutation.
void register_timeline_tools(mcp::Server& server);

}  // namespace et::tools
