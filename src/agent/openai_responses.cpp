#include "agent/openai_responses.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "agent/http.h"

namespace et::agent {

namespace {

std::string string_field(const json& j, const char* key) {
  auto it = j.find(key);
  return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

long long int_field(const json& j, const char* key, long long fallback = 0) {
  auto it = j.find(key);
  if (it == j.end()) return fallback;
  if (it->is_number_unsigned()) return static_cast<long long>(it->get<unsigned long long>());
  if (it->is_number_integer()) return it->get<long long>();
  if (it->is_string()) {
    try {
      return std::stoll(it->get<std::string>());
    } catch (...) {
    }
  }
  return fallback;
}

json input_image(const Image& image) {
  return {{"type", "input_image"}, {"image_url", "data:" + image.mime + ";base64," + image.data}};
}

// Function-call arguments travel as a JSON string. Keep unparseable arguments visible so the model can be
// told about them instead of the call silently disappearing.
json parse_arguments(const std::string& raw) {
  if (raw.empty()) return json::object();
  json parsed = json::parse(raw, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    json out = json::object();
    out["__invalid_arguments"] = raw;
    return out;
  }
  return parsed;
}

void apply_usage(const json& usage, Completion& c) {
  if (!usage.is_object()) return;
  c.input_tokens = static_cast<long>(int_field(usage, "input_tokens", c.input_tokens));
  c.output_tokens = static_cast<long>(int_field(usage, "output_tokens", c.output_tokens));
}

std::string failure_message(const json& j) {
  auto message_of = [](const json& node) -> std::string {
    if (!node.is_object()) return std::string();
    auto it = node.find("message");
    return (it != node.end() && it->is_string()) ? it->get<std::string>() : std::string();
  };
  if (auto error = j.find("error"); error != j.end()) {
    if (error->is_string() && !error->get<std::string>().empty()) return error->get<std::string>();
    std::string msg = message_of(*error);
    if (!msg.empty()) return msg;
  }
  if (auto response = j.find("response"); response != j.end() && response->is_object()) {
    if (auto error = response->find("error"); error != response->end()) {
      std::string msg = message_of(*error);
      if (!msg.empty()) return msg;
    }
  }
  return message_of(j);
}

}  // namespace

OpenAiResponses::OpenAiResponses(ProviderConfig cfg) : cfg_(std::move(cfg)) {
  if (cfg_.api_key.empty()) throw std::invalid_argument("the codex provider needs a signed-in ChatGPT account");
  if (cfg_.base_url.empty()) cfg_.base_url = "https://chatgpt.com/backend-api/codex";
  while (!cfg_.base_url.empty() && cfg_.base_url.back() == '/') cfg_.base_url.pop_back();
  if (cfg_.model.empty()) cfg_.model = "gpt-5.6-sol";  // same default as Codex.cpp
}

json OpenAiResponses::build_request(const std::vector<Message>& messages, const std::vector<ToolSpec>& tools) const {
  std::string instructions;
  for (const Message& m : messages) {
    if (m.role != Message::Role::System) continue;
    if (!instructions.empty()) instructions += "\n\n";
    instructions += m.text;
  }

  json input = json::array();
  std::vector<Image> pending_images;  // tool output images cannot ride in function_call_output
  auto flush_images = [&]() {
    if (pending_images.empty()) return;
    json content = json::array();
    content.push_back({{"type", "input_text"}, {"text", "Images returned by the tool call above"}});
    for (const Image& image : pending_images) content.push_back(input_image(image));
    input.push_back({{"role", "user"}, {"content", content}});
    pending_images.clear();
  };

  for (const Message& m : messages) {
    if (m.role != Message::Role::Tool) flush_images();
    switch (m.role) {
      case Message::Role::System:
        break;  // hoisted into "instructions"
      case Message::Role::User: {
        json content = json::array();
        if (!m.text.empty()) content.push_back({{"type", "input_text"}, {"text", m.text}});
        for (const Image& image : m.images) content.push_back(input_image(image));
        input.push_back({{"role", "user"}, {"content", content}});
        break;
      }
      case Message::Role::Assistant: {
        if (!m.text.empty()) {
          json content = json::array();
          content.push_back({{"type", "output_text"}, {"text", m.text}});
          input.push_back({{"type", "message"}, {"role", "assistant"}, {"content", content}});
        }
        for (const ToolCall& call : m.calls) {
          input.push_back({{"type", "function_call"},
                           {"call_id", call.id},
                           {"name", call.name},
                           {"arguments", call.arguments.dump()}});
        }
        break;
      }
      case Message::Role::Tool: {
        input.push_back({{"type", "function_call_output"}, {"call_id", m.call_id}, {"output", m.text}});
        for (const Image& image : m.images) pending_images.push_back(image);
        break;
      }
    }
  }
  flush_images();

  json tool_specs = json::array();
  for (const ToolSpec& tool : tools) {
    tool_specs.push_back({{"type", "function"},
                          {"name", tool.name},
                          {"description", tool.description},
                          {"parameters", tool.schema}});
  }

  return {{"model", cfg_.model},
          {"instructions", instructions},
          {"input", input},
          {"tools", tool_specs},
          {"stream", true},
          {"store", false},
          {"tool_choice", "auto"},
          {"parallel_tool_calls", true}};
}

void ResponsesStreamParser::feed_line(const std::string& line) {
  if (line.rfind("data:", 0) != 0) return;
  std::string payload = line.substr(5);
  if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
  if (payload == "[DONE]") {
    done_ = true;
    return;
  }
  json j = json::parse(payload, nullptr, false);
  if (!j.is_object()) return;
  const std::string type = string_field(j, "type");

  // Finds (or creates) the slot for a streamed function call. `item_id` is the correlation key for the
  // argument deltas, so records are keyed by it and fall back to the order they appeared.
  auto slot_for = [&](const std::string& item_id) -> size_t {
    if (!item_id.empty()) {
      for (size_t i = 0; i < ids_.size(); ++i)
        if (ids_[i] == item_id) return i;
    } else if (!ids_.empty()) {
      return ids_.size() - 1;  // no correlation key at all: most recent call
    }
    calls_.emplace_back();
    args_.emplace_back();
    ids_.push_back(item_id);
    names_.emplace_back();
    return calls_.size() - 1;
  };

  if (type == "response.output_text.delta") {
    const std::string delta = string_field(j, "delta");
    if (!delta.empty()) {
      c_.text += delta;
      if (on_text_) on_text_(delta);
    }
    return;
  }

  if (type == "response.output_item.added" || type == "response.output_item.done") {
    auto item_it = j.find("item");
    if (item_it == j.end() || !item_it->is_object()) return;
    const json& item = *item_it;
    if (string_field(item, "type") != "function_call") return;

    const std::string call_id = string_field(item, "call_id");
    std::string item_id = string_field(item, "id");
    if (item_id.empty()) item_id = call_id;
    const size_t idx = slot_for(item_id);
    if (call_id.empty()) {
      if (calls_[idx].id.empty()) calls_[idx].id = item_id;
    } else {
      calls_[idx].id = call_id;
    }
    const std::string name = string_field(item, "name");
    if (!name.empty()) names_[idx] = name;

    auto args_it = item.find("arguments");
    if (args_it != item.end()) {
      const std::string arguments = args_it->is_string() ? args_it->get<std::string>() : args_it->dump();
      // A done event carries the final arguments: prefer them over the accumulated deltas.
      if (type == "response.output_item.done" || args_[idx].empty()) args_[idx] = arguments;
    }
    return;
  }

  if (type == "response.function_call_arguments.delta") {
    const std::string delta = string_field(j, "delta");
    if (delta.empty()) return;
    args_[slot_for(string_field(j, "item_id"))] += delta;
    return;
  }

  if (type == "response.completed") {
    auto response_it = j.find("response");
    const json* response = (response_it != j.end() && response_it->is_object()) ? &*response_it : &j;
    auto usage_it = response->find("usage");
    if (usage_it != response->end()) apply_usage(*usage_it, c_);
    if (c_.stop_reason.empty()) c_.stop_reason = "completed";
    done_ = true;
    return;
  }

  if (type == "response.failed" || type == "error") {
    error_ = failure_message(j);
    if (error_.empty()) error_ = "the model request failed";
    c_.stop_reason = "failed";
    done_ = true;
    return;
  }
}

Completion ResponsesStreamParser::finish() {
  Completion out = c_;
  out.calls.clear();
  for (size_t i = 0; i < calls_.size(); ++i) {
    if (i >= names_.size() || names_[i].empty()) continue;  // only completed function calls survive
    ToolCall call = calls_[i];
    call.name = names_[i];
    call.arguments = parse_arguments(i < args_.size() ? args_[i] : std::string());
    if (call.id.empty() && i < ids_.size()) call.id = ids_[i];
    if (call.id.empty()) call.id = "call_" + std::to_string(out.calls.size());
    out.calls.push_back(std::move(call));
  }
  return out;
}

Completion OpenAiResponses::complete(const std::vector<Message>& messages, const std::vector<ToolSpec>& tools,
                                     const CompleteOptions& opt) {
  HttpRequest req;
  req.url = cfg_.base_url + "/responses";
  req.headers = {"Content-Type: application/json", "Accept: text/event-stream",
                 "Authorization: Bearer " + cfg_.api_key};
  if (!cfg_.account.empty()) req.headers.push_back("chatgpt-account-id: " + cfg_.account);
  req.headers.push_back("OpenAI-Beta: responses=experimental");
  req.headers.push_back("originator: " + (cfg_.instructions.empty() ? std::string("editing-agent") : cfg_.instructions));
  req.body = build_request(messages, tools).dump();

  ResponsesStreamParser parser(opt.on_text_delta);
  HttpResult r = http_post_retry(req, [&](const std::string& line) { parser.feed_line(line); }, opt.cancelled, 2, 2.0,
                                 opt.on_retry);

  if (!r.error.empty()) throw ProviderError(r.error, r.status);
  if (!r.ok()) throw ProviderError("HTTP " + std::to_string(r.status) + ": " + error_message_from_body(r.body), r.status);
  if (!parser.error().empty()) throw ProviderError(parser.error(), r.status);

  Completion c = parser.finish();
  if (c.text.empty() && c.calls.empty()) {
    // A server that ignored stream:true answers with one plain JSON document; salvage it.
    json j = json::parse(r.body, nullptr, false);
    if (j.is_object() && j.contains("output") && j["output"].is_array()) {
      for (const json& item : j["output"]) {
        const std::string type = string_field(item, "type");
        if (type == "message" && item.contains("content") && item["content"].is_array()) {
          for (const json& part : item["content"])
            if (string_field(part, "type") == "output_text") c.text += string_field(part, "text");
        } else if (type == "function_call") {
          ToolCall call;
          call.name = string_field(item, "name");
          if (call.name.empty()) continue;
          call.id = string_field(item, "call_id");
          if (call.id.empty()) call.id = string_field(item, "id");
          if (call.id.empty()) call.id = "call_" + std::to_string(c.calls.size());
          call.arguments = parse_arguments(string_field(item, "arguments"));
          c.calls.push_back(std::move(call));
        }
      }
      if (auto usage = j.find("usage"); usage != j.end()) apply_usage(*usage, c);
    }
    if (c.text.empty() && c.calls.empty()) throw ProviderError("the server returned no content", r.status);
  }
  return c;
}

}  // namespace et::agent
