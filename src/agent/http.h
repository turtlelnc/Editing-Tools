#pragma once
#include <functional>
#include <string>
#include <vector>

namespace et::agent {

struct HttpRequest {
  std::string url;
  std::vector<std::string> headers;  // "Name: value"; may hold secrets, so they never go on the command line
  std::string body;
  int timeout_sec = 300;
};

struct HttpResult {
  long status = 0;      // 0 = no HTTP response (connection failure, timeout, cancelled)
  std::string body;     // whole response body (capped)
  std::string error;    // transport-level problem, empty on a normal HTTP exchange
  bool ok() const { return status >= 200 && status < 300 && error.empty(); }
};

// POSTs via the system curl (no shell). If on_line is given it is called with each response line as it arrives
// (without the newline), which is how server-sent events are consumed. `cancelled` is polled to abort early.
HttpResult http_post(const HttpRequest& req, const std::function<void(const std::string&)>& on_line = {},
                     const std::function<bool()>& cancelled = {});

// Same, retrying a few times on 429/502/503/504 and connection failures, with a short backoff.
// `on_retry` is told why and how long it will wait. Never retries once any line has been delivered.
HttpResult http_post_retry(const HttpRequest& req, const std::function<void(const std::string&)>& on_line = {},
                           const std::function<bool()>& cancelled = {}, int max_retries = 2, double backoff_sec = 2.0,
                           const std::function<void(const std::string&)>& on_retry = {});

}  // namespace et::agent
