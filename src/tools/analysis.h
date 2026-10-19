#pragma once
#include "mcp/server.h"

namespace et::tools {

// Analysis tools that look at footage without changing it: scene changes, silence, rough cuts.
void register_analysis_tools(mcp::Server& server);

}  // namespace et::tools
