#include <catch2/catch_test_macros.hpp>
#include <sstream>

#include "mcp/server.h"

using et::mcp::json;

static et::mcp::Server make_server() {
  et::mcp::Server s("test", "0");
  s.add_tool({"echo", "echo", {{"type", "object"}}, [](const json& a) { return a; }});
  s.add_tool({"boom", "fails", {{"type", "object"}}, [](const json&) -> json { throw et::mcp::ToolError("nope"); }});
  return s;
}

TEST_CASE("initialize returns server info and tools capability") {
  auto s = make_server();
  auto r = s.handle({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", {{"protocolVersion", "2025-06-18"}}}});
  REQUIRE(r["result"]["serverInfo"]["name"] == "test");
  REQUIRE(r["result"]["capabilities"].contains("tools"));
}

TEST_CASE("notifications produce no response") {
  auto s = make_server();
  REQUIRE(s.handle({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}).is_null());
}

TEST_CASE("tools/list and tools/call") {
  auto s = make_server();
  auto list = s.handle({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
  REQUIRE(list["result"]["tools"].size() == 2);

  auto ok = s.handle({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"},
                      {"params", {{"name", "echo"}, {"arguments", {{"x", 1}}}}}});
  REQUIRE(ok["result"]["isError"] == false);

  auto bad = s.handle({{"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/call"}, {"params", {{"name", "boom"}}}});
  REQUIRE(bad["result"]["isError"] == true);
  REQUIRE(bad["result"]["content"][0]["text"] == "nope");
}

TEST_CASE("unknown method yields JSON-RPC error; garbage input yields parse error") {
  auto s = make_server();
  REQUIRE(s.handle({{"jsonrpc", "2.0"}, {"id", 5}, {"method", "nope"}})["error"]["code"] == -32601);
  std::istringstream in("not json\n");
  std::ostringstream out;
  s.serve(in, out);
  REQUIRE(json::parse(out.str())["error"]["code"] == -32700);
}
