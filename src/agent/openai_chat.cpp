#include "agent/openai_chat.h"

#include "agent/http.h"

namespace et::agent {

namespace {

json image_part(const Image& im) {
  return {{"type", "image_url"}, {"image_url", {{"url", "data:" + im.mime + ";base64," + im.data}}}};
}

}  // namespace

std::string error_message_from_body(const std::string& body) {
  json j = json::parse(body, nullptr, false);
  if (j.is_object()) {
    if (j.contains("error")) {
      const json& e = j["error"];
      if (e.is_object() && e.contains("message") && e["message"].is_string()) return e["message"];
      if (e.is_string()) return e;
    }
    if (j.contains("message") && j["message"].is_string()) return j["message"];
  }
  std::string s = body.substr(0, 300);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

OpenAiChat::OpenAiChat(ProviderConfig cfg) : cfg_(std::move(cfg)) {
  if (cfg_.base_url.empty()) cfg_.base_url = "https://api.openai.com/v1";
  while (!cfg_.base_url.empty() && cfg_.base_url.back() == '/') cfg_.base_url.pop_back();
  if (cfg_.model.empty()) throw std::invalid_argument("--model is required for the openai provider");
  // Local OpenAI-compatible servers (Ollama, llama.cpp) need no key; the real API does.
  if (cfg_.api_key.empty() && cfg_.base_url.rfind("http://127.0.0.1", 0) != 0 &&
      cfg_.base_url.rfind("http://localhost", 0) != 0)
    throw std::invalid_argument("the openai provider needs OPENAI_API_KEY (or --api-key); for a local server pass --base-url");
}

json OpenAiChat::build_request(const std::vector<Message>& messages, const std::vector<ToolSpec>& tools) const {
  json msgs = json::array();
  std::vector<std::pair<std::string, Image>> pending;  // tool-produced images, to be shown after the tool messages

  auto flush_images = [&]() {
    if (pending.empty()) return;
    json content = json::array();
    content.push_back({{"type", "text"}, {"text", "Images returned by the tool call(s) above:"}});
    for (auto& [tool, im] : pending) {
      content.push_back({{"type", "text"}, {"text", "Output image from " + tool + ":"}});
      content.push_back(image_part(im));
    }
    msgs.push_back({{"role", "user"}, {"content", content}});
    pending.clear();
  };

  for (auto& m : messages) {
    if (m.role != Message::Role::Tool) flush_images();
    switch (m.role) {
      case Message::Role::System:
        msgs.push_back({{"role", "system"}, {"content", m.text}});
        break;
      case Message::Role::User: {
        if (m.images.empty()) {
          msgs.push_back({{"role", "user"}, {"content", m.text}});
        } else {
          json content = json::array();
          if (!m.text.empty()) content.push_back({{"type", "text"}, {"text", m.text}});
          for (auto& im : m.images) content.push_back(image_part(im));
          msgs.push_back({{"role", "user"}, {"content", content}});
        }
        break;
      }
      case Message::Role::Assistant: {
        json a = {{"role", "assistant"}, {"content", m.text.empty() ? json(nullptr) : json(m.text)}};
        if (!m.calls.empty()) {
          json calls = json::array();
          for (auto& c : m.calls)
            calls.push_back({{"id", c.id}, {"type", "function"}, {"function", {{"name", c.name}, {"arguments", c.arguments.dump()}}}});
          a["tool_calls"] = calls;
        }
        msgs.push_back(a);
        break;
      }
      case Message::Role::Tool:
        msgs.push_back({{"role", "tool"}, {"tool_call_id", m.call_id}, {"content", m.text}});
        for (auto& im : m.images) pending.emplace_back(m.tool_name, im);
        break;
    }
  }
  flush_images();

  json req = {{"model", cfg_.model}, {"messages", msgs}, {"stream", true}, {"stream_options", {{"include_usage", true}}}};
  if (!tools.empty()) {
    json ts = json::array();
    for (auto& t : tools)
      ts.push_back({{"type", "function"}, {"function", {{"name", t.name}, {"description", t.description}, {"parameters", t.schema}}}});
    req["tools"] = ts;
  }
  return req;
}

void ChatStreamParser::feed_line(const std::string& line) {
  if (line.rfind("data:", 0) != 0) return;
  std::string payload = line.substr(5);
  if (!payload.empty() && payload[0] == ' ') payload.erase(0, 1);
  if (payload == "[DONE]") { done_ = true; return; }
  json j = json::parse(payload, nullptr, false);
  if (!j.is_object()) return;

  if (j.contains("usage") && j["usage"].is_object()) {
    c_.input_tokens = j["usage"].value("prompt_tokens", 0L);
    c_.output_tokens = j["usage"].value("completion_tokens", 0L);
  }
  if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) return;
  const json& choice = j["choices"][0];
  if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) c_.stop_reason = choice["finish_reason"];
  if (!choice.contains("delta") || !choice["delta"].is_object()) return;
  const json& d = choice["delta"];

  if (d.contains("content") && d["content"].is_string() && !d["content"].get<std::string>().empty()) {
    c_.text += d["content"].get<std::string>();
    if (on_text_) on_text_(d["content"].get<std::string>());
  }
  if (d.contains("tool_calls") && d["tool_calls"].is_array()) {
    for (const json& tc : d["tool_calls"]) {
      size_t idx = tc.value("index", 0);
      if (idx >= 64) continue;
      if (calls_.size() <= idx) { calls_.resize(idx + 1); args_.resize(idx + 1); }
      if (tc.contains("id") && tc["id"].is_string()) calls_[idx].id = tc["id"];
      if (tc.contains("function") && tc["function"].is_object()) {
        const json& f = tc["function"];
        if (f.contains("name") && f["name"].is_string()) calls_[idx].name += f["name"].get<std::string>();
        if (f.contains("arguments") && f["arguments"].is_string()) args_[idx] += f["arguments"].get<std::string>();
      }
    }
  }
}

Completion ChatStreamParser::finish() {
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

Completion OpenAiChat::complete(const std::vector<Message>& messages, const std::vector<ToolSpec>& tools, const CompleteOptions& opt) {
  HttpRequest req;
  req.url = cfg_.base_url + "/chat/completions";
  req.headers = {"Content-Type: application/json", "Accept: text/event-stream"};
  if (!cfg_.api_key.empty()) req.headers.push_back("Authorization: Bearer " + cfg_.api_key);
  req.body = build_request(messages, tools).dump();

  ChatStreamParser parser(opt.on_text_delta);
  HttpResult r = http_post_retry(req, [&](const std::string& line) { parser.feed_line(line); }, opt.cancelled, 2, 2.0, opt.on_retry);

  if (!r.error.empty()) throw ProviderError(r.error, r.status);
  if (!r.ok()) throw ProviderError("HTTP " + std::to_string(r.status) + ": " + error_message_from_body(r.body), r.status);
  Completion c = parser.finish();
  if (!parser.done() && c.text.empty() && c.calls.empty()) {
    // A server that ignored stream:true answers with one plain JSON document; salvage it.
    json j = json::parse(r.body, nullptr, false);
    if (j.is_object() && j.contains("choices") && !j["choices"].empty()) {
      const json& msg = j["choices"][0].value("message", json::object());
      if (msg.contains("content") && msg["content"].is_string()) c.text = msg["content"];
      if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
        for (const json& tc : msg["tool_calls"]) {
          ToolCall call;
          call.id = tc.value("id", "call_" + std::to_string(c.calls.size()));
          call.name = tc["function"].value("name", "");
          json a = json::parse(tc["function"].value("arguments", "{}"), nullptr, false);
          call.arguments = a.is_object() ? a : json::object();
          c.calls.push_back(call);
        }
      }
    }
    if (c.text.empty() && c.calls.empty()) throw ProviderError("the server returned no content", r.status);
  }
  return c;
}

}  // namespace et::agent
