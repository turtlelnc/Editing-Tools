#include "agent/anthropic.h"

#include <utility>

#include "agent/http.h"

namespace et::agent {

namespace {

constexpr const char* kDefaultBaseUrl = "https://api.anthropic.com";
constexpr const char* kDefaultModel = "claude-sonnet-5-5";
constexpr const char* kApiVersion = "2023-06-01";

json image_block(const Image& im) {
  return {{"type", "image"}, {"source", {{"type", "base64"}, {"media_type", im.mime}, {"data", im.data}}}};
}

// A user message with no text and no images still needs one (empty) block.
json content_blocks(const std::string& text, const std::vector<Image>& images) {
  json content = json::array();
  if (!text.empty() || images.empty()) content.push_back({{"type", "text"}, {"text", text}});
  for (const auto& im : images) content.push_back(image_block(im));
  return content;
}

// Both the streaming events and a non-streamed document report usage the same way.
void apply_usage(const json& usage, long& input_tokens, long& output_tokens) {
  if (!usage.is_object()) return;
  if (usage.contains("input_tokens") && usage["input_tokens"].is_number())
    input_tokens = usage["input_tokens"].get<long>();
  if (usage.contains("output_tokens") && usage["output_tokens"].is_number())
    output_tokens = usage["output_tokens"].get<long>();
}

}  // namespace

Anthropic::Anthropic(ProviderConfig cfg) : cfg_(std::move(cfg)) {
  if (cfg_.base_url.empty()) cfg_.base_url = kDefaultBaseUrl;
  while (!cfg_.base_url.empty() && cfg_.base_url.back() == '/') cfg_.base_url.pop_back();
  if (cfg_.model.empty()) cfg_.model = kDefaultModel;
  if (cfg_.api_key.empty()) throw std::invalid_argument("the anthropic provider needs ANTHROPIC_API_KEY");
}

json Anthropic::build_request(const std::vector<Message>& messages, const std::vector<ToolSpec>& tools) const {
  json req = {{"model", cfg_.model}, {"max_tokens", kMaxTokens}, {"stream", true}};

  // Anthropic has no system role: all system text is hoisted into one top-level string.
  std::string system;
  for (const auto& m : messages) {
    if (m.role != Message::Role::System || m.text.empty()) continue;
    if (!system.empty()) system += "\n\n";
    system += m.text;
  }
  if (!system.empty()) req["system"] = system;

  json msgs = json::array();
  size_t open_tool_message = 0;  // >0: index+1 of the user message collecting tool_result blocks
  for (const auto& m : messages) {
    switch (m.role) {
      case Message::Role::System:
        break;
      case Message::Role::User:
        open_tool_message = 0;
        msgs.push_back({{"role", "user"}, {"content", content_blocks(m.text, m.images)}});
        break;
      case Message::Role::Assistant: {
        open_tool_message = 0;
        json content = json::array();
        if (!m.text.empty()) content.push_back({{"type", "text"}, {"text", m.text}});
        for (const auto& c : m.calls)
          content.push_back({{"type", "tool_use"},
                             {"id", c.id},
                             {"name", c.name},
                             {"input", c.arguments.is_object() ? c.arguments : json::object()}});
        if (content.empty()) break;  // a turn with neither text nor calls has nothing to send
        msgs.push_back({{"role", "assistant"}, {"content", content}});
        break;
      }
      case Message::Role::Tool: {
        // Tool output is a tool_result block inside a *user* message, and every result of one assistant
        // turn has to share that user message, so consecutive Tool messages are merged.
        json content = content_blocks(m.text, m.images);
        json block = {{"type", "tool_result"},
                      {"tool_use_id", m.call_id},
                      {"is_error", m.is_error},
                      {"content", content}};
        if (open_tool_message == 0) {
          msgs.push_back({{"role", "user"}, {"content", json::array({block})}});
          open_tool_message = msgs.size();
        } else {
          msgs[open_tool_message - 1]["content"].push_back(block);
        }
        break;
      }
    }
  }
  req["messages"] = msgs;

  if (!tools.empty()) {
    json specs = json::array();
    for (const auto& t : tools) {
      json schema = t.schema.is_object() ? t.schema : json{{"type", "object"}, {"properties", json::object()}};
      specs.push_back({{"name", t.name}, {"description", t.description}, {"input_schema", schema}});
    }
    req["tools"] = specs;
  }
  return req;
}

void AnthropicStreamParser::append_text(const std::string& text) {
  if (text.empty()) return;
  c_.text += text;
  if (on_text_) on_text_(text);
}

void AnthropicStreamParser::feed_line(const std::string& line) {
  if (line.empty()) return;
  if (line.rfind("event:", 0) == 0) {
    event_ = line.substr(6);
    while (!event_.empty() && (event_.front() == ' ' || event_.front() == '\t')) event_.erase(event_.begin());
    return;
  }
  if (line.rfind("data:", 0) != 0) return;

  std::string payload = line.substr(5);
  if (!payload.empty() && payload[0] == ' ') payload.erase(0, 1);
  while (!payload.empty() && (payload.back() == '\r' || payload.back() == '\n')) payload.pop_back();
  json j = json::parse(payload, nullptr, false);
  if (!j.is_object()) return;

  std::string type = j.value("type", std::string());
  if (type.empty()) type = event_;

  if (type == "message_start") {
    if (j.contains("message") && j["message"].is_object()) {
      const json& msg = j["message"];
      if (msg.contains("usage")) apply_usage(msg["usage"], c_.input_tokens, c_.output_tokens);
      if (msg.contains("stop_reason") && msg["stop_reason"].is_string()) c_.stop_reason = msg["stop_reason"];
    }
    return;
  }

  if (type == "content_block_start") {
    int index = j.contains("index") && j["index"].is_number_integer() ? j["index"].get<int>()
                                                                     : static_cast<int>(calls_.size());
    if (index < 0 || index > 1024) return;
    current_block_ = index;
    current_tool_.clear();
    if (!j.contains("content_block") || !j["content_block"].is_object()) return;
    const json& block = j["content_block"];
    const std::string kind = block.value("type", std::string());
    if (kind == "tool_use") {
      if (calls_.size() <= static_cast<size_t>(index)) {
        calls_.resize(static_cast<size_t>(index) + 1);
        args_.resize(static_cast<size_t>(index) + 1);
      }
      ToolCall& call = calls_[static_cast<size_t>(index)];
      if (block.contains("id") && block["id"].is_string()) call.id = block["id"];
      if (block.contains("name") && block["name"].is_string()) call.name = block["name"];
      current_tool_ = call.name;
      // Real servers start a tool_use block with "input": {} and stream the JSON in input_json_delta chunks; a
      // non-empty input here means the whole object was sent up front instead.
      if (args_[static_cast<size_t>(index)].empty() && block.contains("input") && block["input"].is_object() &&
          !block["input"].empty())
        args_[static_cast<size_t>(index)] = block["input"].dump();
    } else if (kind == "text" && block.contains("text") && block["text"].is_string()) {
      append_text(block["text"].get<std::string>());
    }
    return;
  }

  if (type == "content_block_delta") {
    int index = j.contains("index") && j["index"].is_number_integer() ? j["index"].get<int>() : current_block_;
    if (index < 0 || index > 1024) return;
    if (calls_.size() <= static_cast<size_t>(index)) {
      calls_.resize(static_cast<size_t>(index) + 1);
      args_.resize(static_cast<size_t>(index) + 1);
    }
    if (!j.contains("delta") || !j["delta"].is_object()) return;
    const json& delta = j["delta"];
    const std::string kind = delta.value("type", std::string());
    if (kind == "text_delta") {
      if (delta.contains("text") && delta["text"].is_string()) append_text(delta["text"].get<std::string>());
    } else if (kind == "input_json_delta") {
      if (delta.contains("partial_json") && delta["partial_json"].is_string())
        args_[static_cast<size_t>(index)] += delta["partial_json"].get<std::string>();
    }
    return;
  }

  if (type == "content_block_stop") {
    current_block_ = -1;
    current_tool_.clear();
    return;
  }

  if (type == "message_delta") {
    if (j.contains("delta") && j["delta"].is_object()) {
      const json& delta = j["delta"];
      if (delta.contains("stop_reason") && delta["stop_reason"].is_string()) c_.stop_reason = delta["stop_reason"];
    }
    if (j.contains("usage")) apply_usage(j["usage"], c_.input_tokens, c_.output_tokens);
    return;
  }

  if (type == "message_stop") {
    done_ = true;
    return;
  }

  if (type == "error") {
    error_ = error_message_from_body(j.dump());
    if (error_.empty()) error_ = "the model stream reported an error";
    done_ = true;
    return;
  }
}

Completion AnthropicStreamParser::finish() {
  for (size_t i = 0; i < calls_.size(); ++i) {
    if (calls_[i].name.empty()) continue;
    json args = args_[i].empty() ? json::object() : json::parse(args_[i], nullptr, false);
    if (args.is_discarded() || !args.is_object()) {
      // Keep the call so the model sees an error for it rather than silently losing the turn.
      calls_[i].arguments = json::object();
      calls_[i].arguments["__invalid_arguments"] = args_[i];
    } else {
      calls_[i].arguments = args;
    }
    if (calls_[i].id.empty()) calls_[i].id = "call_" + std::to_string(i);
    c_.calls.push_back(calls_[i]);
  }
  return c_;
}

Completion Anthropic::complete(const std::vector<Message>& messages, const std::vector<ToolSpec>& tools,
                               const CompleteOptions& opt) {
  HttpRequest req;
  req.url = cfg_.base_url + "/v1/messages";
  req.headers = {"Content-Type: application/json", "Accept: text/event-stream", "x-api-key: " + cfg_.api_key,
                 "anthropic-version: " + std::string(kApiVersion)};
  req.body = build_request(messages, tools).dump();

  AnthropicStreamParser parser(opt.on_text_delta);
  HttpResult r = http_post_retry(req, [&](const std::string& line) { parser.feed_line(line); }, opt.cancelled, 2, 2.0,
                                 opt.on_retry);

  if (!r.error.empty()) throw ProviderError(r.error, r.status);
  if (!r.ok()) throw ProviderError("HTTP " + std::to_string(r.status) + ": " + error_message_from_body(r.body), r.status);

  Completion c = parser.finish();
  if (!parser.error().empty()) throw ProviderError(parser.error(), r.status);
  if (!parser.done() && c.text.empty() && c.calls.empty()) {
    // A server that ignored stream:true answers with one plain JSON message document; salvage it.
    json j = json::parse(r.body, nullptr, false);
    if (j.is_object() && j.contains("content") && j["content"].is_array()) {
      for (const json& block : j["content"]) {
        if (!block.is_object()) continue;
        const std::string kind = block.value("type", std::string());
        if (kind == "text") {
          if (block.contains("text") && block["text"].is_string()) c.text += block["text"].get<std::string>();
          continue;
        }
        if (kind != "tool_use") continue;
        ToolCall call;
        call.name = block.value("name", std::string());
        if (call.name.empty()) continue;
        call.id = block.contains("id") && block["id"].is_string() ? block["id"].get<std::string>()
                                                                 : "call_" + std::to_string(c.calls.size());
        if (block.contains("input") && block["input"].is_object()) call.arguments = block["input"];
        c.calls.push_back(std::move(call));
      }
      if (j.contains("usage")) apply_usage(j["usage"], c.input_tokens, c.output_tokens);
      if (c.stop_reason.empty() && j.contains("stop_reason") && j["stop_reason"].is_string())
        c.stop_reason = j["stop_reason"];
    }
  }
  if (c.text.empty() && c.calls.empty()) {
    json j = json::parse(r.body, nullptr, false);
    if (j.is_object() && j.contains("error")) throw ProviderError(error_message_from_body(r.body), r.status);
    throw ProviderError("the server returned no content", r.status);
  }
  return c;
}

}  // namespace et::agent
