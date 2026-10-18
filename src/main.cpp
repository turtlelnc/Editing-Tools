#include <unistd.h>

#include <iostream>

#include "cli/cli.h"
#include "mcp/server.h"
#include "tools/all.h"

int main(int argc, char** argv) {
  et::mcp::Server server("editing-tools", "0.1.0");
  auto session = std::make_shared<et::tools::Session>();
  et::tools::register_all_tools(server, session);

  std::vector<std::string> args(argv + 1, argv + argc);
  const bool serve = (args.empty() && !isatty(STDIN_FILENO)) || (!args.empty() && args[0] == "mcp");
  if (serve) {
    std::ios::sync_with_stdio(false);
    server.serve(std::cin, std::cout);  // stdout is the protocol channel; log to stderr only
    return 0;
  }
  return et::cli::run(args, std::cin, std::cout, std::cerr, server, session);
}
