#include <catch2/catch_test_macros.hpp>
#include <set>

#include "mcp/server.h"
#include "tools/all.h"

using et::mcp::json;

// Real MCP clients validate tools/list strictly and drop the whole server on one bad schema.
TEST_CASE("every tool advertises a well-formed input schema") {
  et::mcp::Server server("t", "0");
  auto session = std::make_shared<et::tools::Session>();
  et::tools::register_all_tools(server, session);

  auto tools = server.handle({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}})["result"]["tools"];
  REQUIRE(tools.size() >= 20);
  std::set<std::string> names;
  for (auto& t : tools) {
    INFO("tool: " << t["name"]);
    REQUIRE(names.insert(t["name"]).second);  // unique
    REQUIRE_FALSE(t["description"].get<std::string>().empty());
    auto& s = t["inputSchema"];
    REQUIRE(s["type"] == "object");
    REQUIRE(s["properties"].is_object());
    REQUIRE(s["required"].is_array());
    for (auto& r : s["required"]) REQUIRE(s["properties"].contains(r.get<std::string>()));
    for (auto& [k, v] : s["properties"].items()) {
      INFO("property: " << k);
      REQUIRE(v.is_object());
      REQUIRE(v.contains("type"));
    }
  }
}
