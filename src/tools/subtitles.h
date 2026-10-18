#pragma once
#include "mcp/server.h"
#include "tools/session.h"

namespace et::tools {

void register_subtitle_tools(mcp::Server& server, std::shared_ptr<Session> session);

}  // namespace et::tools
