#pragma once
#include "mcp/server.h"
#include "tools/session.h"

namespace et::tools {

// Registers every tool. Shared by the MCP server, the CLI and the tests.
void register_all_tools(mcp::Server& server, std::shared_ptr<Session> session);

}  // namespace et::tools
