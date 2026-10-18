#include <iostream>

#include "mcp/server.h"
#include "tools/media.h"
#include "tools/subtitles.h"
#include "tools/timeline.h"

int main() {
  std::ios::sync_with_stdio(false);
  et::mcp::Server server("editing-tools", "0.1.0");
  et::tools::register_media_tools(server);
  auto session = std::make_shared<et::tools::Session>();
  et::tools::register_timeline_tools(server, session);
  et::tools::register_subtitle_tools(server, session);
  server.serve(std::cin, std::cout);  // stdout is the protocol channel; log to stderr only
  return 0;
}
