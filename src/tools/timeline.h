#pragma once
#include "mcp/server.h"
#include "tools/session.h"

namespace et::tools {

// Registers project/timeline/render tools. The project lives in memory and is
// saved to its project.json after every mutation.
void register_timeline_tools(mcp::Server& server, std::shared_ptr<Session> session);

}  // namespace et::tools
