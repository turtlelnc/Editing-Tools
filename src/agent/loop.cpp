#include "agent/loop.h"

#include <algorithm>
#include <stdexcept>

#include "util/activity.h"
#include "util/utf8.h"

namespace et::agent {

namespace {

constexpr size_t kImageChars = 1500;     // rough context cost of one image
constexpr size_t kToolTextLimit = 3000;  // per tool result, when building the summary transcript

std::string truncate(const std::string& s, size_t n) {
  if (s.size() <= n) return s;
  return util::truncate_utf8(s, n) + "…[+" + std::to_string(s.size() - n) + " chars]";
}

const char* role_name(Message::Role r) {
  switch (r) {
    case Message::Role::System: return "system";
    case Message::Role::User: return "user";
    case Message::Role::Assistant: return "assistant";
    case Message::Role::Tool: return "tool";
  }
  return "?";
}

std::string transcript(const std::vector<Message>& messages, size_t begin, size_t end) {
  std::string out;
  for (size_t i = begin; i < end; ++i) {
    const Message& m = messages[i];
    out += "[";
    out += role_name(m.role);
    if (m.role == Message::Role::Tool && !m.tool_name.empty()) out += " " + m.tool_name;
    out += "] ";
    if (!m.text.empty()) out += truncate(m.text, kToolTextLimit);
    for (const auto& c : m.calls) out += "\n  (asked for " + c.name + " " + truncate(util::dump_json(c.arguments), 800) + ")";
    if (!m.images.empty()) out += " [" + std::to_string(m.images.size()) + " image(s)]";
    out += "\n";
  }
  return out;
}

}  // namespace

AgentLoop::AgentLoop(Provider& provider, ToolBox& toolbox, LoopConfig cfg)
    : provider_(provider), toolbox_(toolbox), cfg_(cfg) {}

size_t AgentLoop::usage_chars(const std::vector<Message>& messages) const {
  size_t total = 0;
  for (const auto& m : messages) {
    total += m.text.size() + m.tool_name.size() + m.call_id.size() + m.images.size() * kImageChars;
    for (const auto& c : m.calls) total += c.name.size() + util::dump_json(c.arguments).size() + 32;
    total += 16;
  }
  return total;
}

void AgentLoop::drop_old_images(std::vector<Message>& messages) {
  size_t kept = 0;
  for (size_t i = messages.size(); i-- > 0;) {
    Message& m = messages[i];
    if (m.role != Message::Role::Tool || m.images.empty()) continue;  // user-supplied images always stay
    for (size_t k = m.images.size(); k-- > 0;) {
      if (kept < 2) {
        ++kept;
      } else {
        m.images.erase(m.images.begin() + static_cast<std::ptrdiff_t>(k));
      }
    }
  }
}

void AgentLoop::compact(std::vector<Message>& messages, const LoopEvents& events) {
  if (messages.size() <= cfg_.keep_recent_messages + 3) return;
  if (usage_chars(messages) < cfg_.compact_after_chars) return;

  // Cut only at a user message so the kept conversation still starts a turn and tool results keep their calls.
  size_t cut = messages.size() - cfg_.keep_recent_messages;
  while (cut < messages.size() && messages[cut].role != Message::Role::User) ++cut;
  if (cut < 3 || cut >= messages.size()) return;

  if (events.notice) events.notice("conversation is getting long; summarizing " + std::to_string(cut - 1) + " earlier messages");

  std::vector<Message> ask;
  Message sys;
  sys.role = Message::Role::System;
  sys.text =
      "You compress the log of an AI video-editing session so the work can continue in a fresh context. Keep every "
      "fact needed to continue: the user's goal and constraints, the project file path, each media file used (with its "
      "path), the edits made and their parameters, subtitle text already agreed on, output files already produced, and "
      "what is still pending. Drop chit-chat and repeated tool output. Write plain text under 400 words, no preamble.";
  ask.push_back(std::move(sys));
  Message user;
  user.role = Message::Role::User;
  user.text = "Session so far:\n\n" + transcript(messages, 1, cut);
  ask.push_back(std::move(user));

  Completion c;
  try {
    c = provider_.complete(ask, {}, CompleteOptions{});
  } catch (const std::exception& e) {
    if (events.notice) events.notice(std::string("could not summarize old turns (continuing anyway): ") + e.what());
    return;
  }
  if (c.text.empty()) return;

  Message summary;
  summary.role = Message::Role::User;
  summary.text = "[Summary of the earlier conversation, for context only — do not repeat it to the user]\n" + c.text;
  messages.erase(messages.begin() + 1, messages.begin() + static_cast<std::ptrdiff_t>(cut));
  messages.insert(messages.begin() + 1, std::move(summary));
}

LoopResult AgentLoop::run(std::vector<Message>& messages, const LoopEvents& events) {
  LoopResult result;
  if (messages.empty()) throw std::invalid_argument("the conversation is empty");
  if (cancelled_) {
    result.cancelled = true;
    return result;
  }

  auto append = [&](Message message) {
    messages.push_back(std::move(message));
    if (events.on_message) events.on_message(messages.back());
  };

  long tokens_used = 0;
  int consecutive_errors = 0;
  std::string last_failure;
  int repeats = 0;

  auto stop_reason = [&]() -> std::string {
    if (result.hit_budget) return "token budget";
    if (result.hit_error_limit) return "repeated tool failures";
    if (cancelled_) return "cancelled";
    return "step limit";
  };

  for (int step = 1; step <= cfg_.max_steps; ++step) {
    if (cancelled_) {
      result.cancelled = true;
      return result;
    }
    drop_old_images(messages);
    compact(messages, events);

    CompleteOptions opt;
    opt.on_text_delta = events.text_delta;
    opt.cancelled = [this] { return cancelled_.load(); };
    opt.on_retry = [&events](const std::string& why) {
      if (events.notice) events.notice("model request failed (" + why + ")");
    };

    Completion c = provider_.complete(messages, toolbox_.specs(), opt);
    result.steps = step;
    result.input_tokens += c.input_tokens;
    result.output_tokens += c.output_tokens;
    tokens_used += c.input_tokens + c.output_tokens;

    Message assistant;
    assistant.role = Message::Role::Assistant;
    assistant.text = c.text;
    assistant.calls = c.calls;
    append(std::move(assistant));

    if (c.calls.empty()) {
      result.final_text = c.text;
      return result;
    }

    // A budget stop still has to answer the calls it refuses to run, or the next request would be invalid.
    auto skip_remaining = [&](const std::string& why) {
      for (const ToolCall& call : c.calls) {
        Message skipped;
        skipped.role = Message::Role::Tool;
        skipped.call_id = call.id;
        skipped.tool_name = call.name;
        skipped.text = "not run: " + why;
        skipped.is_error = true;
        append(std::move(skipped));
      }
    };

    if (cfg_.max_tokens > 0 && tokens_used >= cfg_.max_tokens) {
      result.hit_budget = true;
      if (events.notice) events.notice("token budget reached (" + std::to_string(tokens_used) + "); wrapping up");
      skip_remaining("the token budget for this turn was reached");
      break;
    }

    for (const ToolCall& call : c.calls) {
      if (cancelled_) {
        // Keep the log valid: every call still gets an answer.
        Message skipped;
        skipped.role = Message::Role::Tool;
        skipped.call_id = call.id;
        skipped.tool_name = call.name;
        skipped.text = "not run: the user cancelled";
        skipped.is_error = true;
        append(std::move(skipped));
        result.cancelled = true;
        return result;
      }
      if (events.tool_start) events.tool_start(call);
      // Renders and analysis report progress through this hook, and stop when the turn is cancelled.
      activity::Scope activity(events.notice, [this] { return cancelled_.load(); });
      ToolResult tr = toolbox_.execute(call);
      if (events.tool_done) events.tool_done(call, tr);

      Message tm;
      tm.role = Message::Role::Tool;
      tm.call_id = call.id;
      tm.tool_name = call.name;
      tm.text = tr.text;
      tm.images = tr.images;
      tm.is_error = tr.is_error;
      if (tr.is_error) {
        const std::string signature = call.name + "\x1f" + util::dump_json(call.arguments);
        if (signature == last_failure) {
          ++repeats;
        } else {
          last_failure = signature;
          repeats = 1;
        }
        if (repeats >= 2)
          tm.text += "\n[hint: this exact call has now failed " + std::to_string(repeats) +
                     " times in this turn. Change the arguments or the approach before trying again.]";
        ++consecutive_errors;
      } else {
        consecutive_errors = 0;
        last_failure.clear();
        repeats = 0;
      }
      append(std::move(tm));
    }

    if (consecutive_errors >= cfg_.max_consecutive_errors) {
      result.hit_error_limit = true;
      if (events.notice) events.notice("too many failed tool calls in a row; wrapping up");
      break;
    }
    if (cancelled_) {
      result.cancelled = true;
      return result;
    }
  }
  if (cancelled_) {
    result.cancelled = true;
    return result;
  }
  if (result.steps >= cfg_.max_steps && !result.hit_budget && !result.hit_error_limit) result.hit_max_steps = true;

  // One last call without tools, so the turn ends with words instead of a dangling request.
  if (events.notice) events.notice("wrapping up (" + stop_reason() + ")");
  CompleteOptions opt;
  opt.on_text_delta = events.text_delta;
  opt.cancelled = [this] { return cancelled_.load(); };
  Completion c = provider_.complete(messages, {}, opt);
  result.input_tokens += c.input_tokens;
  result.output_tokens += c.output_tokens;
  Message assistant;
  assistant.role = Message::Role::Assistant;
  assistant.text = c.text;
  append(std::move(assistant));
  result.final_text = c.text;
  result.steps += 1;
  return result;
}

}  // namespace et::agent
