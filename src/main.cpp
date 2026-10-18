#include <iostream>

#include "mcp/server.h"
#include "tools/media.h"

int main() {
  std::ios::sync_with_stdio(false);
  et::mcp::Server server("editing-tools", "0.1.0");
  et::tools::register_media_tools(server);
  server.serve(std::cin, std::cout);  // stdout is the protocol channel; log to stderr only
  return 0;
}
