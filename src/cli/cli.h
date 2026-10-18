#pragma once
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "mcp/server.h"
#include "tools/session.h"

namespace et::cli {

// Runs one command-line invocation (args exclude argv[0]) against the registered tools.
// Exit codes: 0 ok, 1 the tool failed, 2 bad usage. Results go to `out`, errors to `err`.
int run(const std::vector<std::string>& args, std::istream& in, std::ostream& out, std::ostream& err,
        const mcp::Server& server, std::shared_ptr<tools::Session> session);

}  // namespace et::cli
