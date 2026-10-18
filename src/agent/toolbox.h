#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "agent/message.h"
#include "agent/policy.h"
#include "mcp/server.h"
#include "tools/session.h"

namespace et::agent {

struct ToolResult {
  std::string text;
  std::vector<Image> images;
  bool is_error = false;
};

// Turns a full project JSON (as the editing tools return it) into a short one that drops default values,
// so long sessions do not drown the model's context in repeated timelines.
json compact_project(const json& project);

// The agent's tools: every editing tool, run in-process behind the policy, plus read-only file discovery.
class ToolBox {
 public:
  ToolBox(std::shared_ptr<tools::Session> session, Policy policy);

  std::vector<ToolSpec> specs() const;
  ToolResult execute(const ToolCall& call);

  // Called before a tool that produces a finished file (render_export), so the front end can ask the user.
  // Returning false refuses the call and `reason` is shown to the model.
  using Approver = std::function<bool(const std::string& tool, const json& args, std::string& reason)>;
  void set_approver(Approver approver) { approver_ = std::move(approver); }

  // Opens an existing project file as the current project (used for --project).
  void open_project(const std::filesystem::path& file) { session_->open(file.string()); }
  bool has_project() const { return session_->project != nullptr; }
  const Policy& policy() const { return policy_; }
  tools::Session& session() { return *session_; }

  void add_tool(mcp::Tool tool) { server_.add_tool(std::move(tool)); }

 private:
  ToolResult run(const ToolCall& call);

  std::shared_ptr<tools::Session> session_;
  Policy policy_;
  mcp::Server server_;
  Approver approver_;
};

}  // namespace et::agent
