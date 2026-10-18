#include "agent/http.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "process.h"

namespace et::agent {

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxBody = 8'000'000;
constexpr size_t kMaxLine = 4'000'000;

// A private temp file: created exclusively with mode 0600, removed on destruction.
struct SecretFile {
  fs::path path;
  explicit SecretFile(const std::string& content) {
    static std::atomic<int> counter{0};
    path = fs::temp_directory_path() / ("et_http_" + std::to_string(getpid()) + "_" + std::to_string(counter++));
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) throw std::runtime_error("cannot create temp file " + path.string());
    size_t off = 0;
    while (off < content.size()) {
      ssize_t n = ::write(fd, content.data() + off, content.size() - off);
      if (n <= 0) break;
      off += static_cast<size_t>(n);
    }
    ::close(fd);
  }
  ~SecretFile() {
    std::error_code ec;
    fs::remove(path, ec);
  }
};

bool is_loopback(const std::string& url) {
  return url.rfind("http://127.0.0.1", 0) == 0 || url.rfind("http://localhost", 0) == 0 || url.rfind("http://[::1]", 0) == 0;
}

}  // namespace

HttpResult http_post(const HttpRequest& req, const std::function<void(const std::string&)>& on_line,
                     const std::function<bool()>& cancelled) {
  HttpResult result;
  std::string header_text;
  for (auto& h : req.headers) header_text += h + "\n";
  SecretFile headers(header_text), body(req.body);

  const std::string marker = "__ET_HTTP_STATUS__:";
  std::vector<std::string> argv = {"curl", "-sS", "-N", "--connect-timeout", "20", "--max-time", std::to_string(req.timeout_sec),
                                   "--max-redirs", "0", "-w", "\n" + marker + "%{http_code}\n", "-H", "@" + headers.path.string(),
                                   "--data-binary", "@" + body.path.string()};
  if (is_loopback(req.url)) argv.insert(argv.end(), {"--noproxy", "*"});  // never send local mock/test traffic to a proxy
  argv.push_back(req.url);

  std::string line;
  auto consume_line = [&]() {
    if (line.rfind(marker, 0) == 0) {
      try { result.status = std::stol(line.substr(marker.size())); } catch (...) {}
    } else {
      if (result.body.size() + line.size() + 1 <= kMaxBody) result.body += line + "\n";
      if (on_line) on_line(line);
    }
    line.clear();
  };
  auto on_stdout = [&](const char* data, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      if (data[i] == '\n') {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        consume_line();
      } else if (line.size() < kMaxLine) {
        line += data[i];
      } else {
        result.error = "response line too long";
        return false;
      }
    }
    return true;
  };

  StreamResult r = run_streaming(argv, on_stdout, cancelled);
  if (!line.empty() && result.error.empty()) consume_line();
  if (r.cancelled && result.error.empty()) result.error = "cancelled";
  if (result.error.empty() && r.exit_code != 0) {
    std::string e = r.err;
    while (!e.empty() && (e.back() == '\n' || e.back() == '\r')) e.pop_back();
    result.error = r.exit_code == 127 ? "curl is not installed" : "curl failed: " + e;
  }
  return result;
}

HttpResult http_post_retry(const HttpRequest& req, const std::function<void(const std::string&)>& on_line,
                           const std::function<bool()>& cancelled, int max_retries, double backoff_sec,
                           const std::function<void(const std::string&)>& on_retry) {
  for (int attempt = 0;; ++attempt) {
    bool delivered = false;
    auto counting = [&](const std::string& l) {
      delivered = true;
      if (on_line) on_line(l);
    };
    HttpResult r = http_post(req, counting, cancelled);
    const bool transient = r.status == 429 || r.status == 502 || r.status == 503 || r.status == 504 ||
                           (r.status == 0 && r.error.rfind("curl failed:", 0) == 0);
    const bool streamed_ok = r.ok() && delivered;
    if (!transient || streamed_ok || attempt >= max_retries || (cancelled && cancelled())) return r;
    // A 429/5xx body is an error document, not stream content: only retry if nothing was forwarded as data.
    if (delivered && r.status == 0) return r;
    const double wait = backoff_sec * (attempt + 1);
    if (on_retry) on_retry("HTTP " + std::to_string(r.status) + (r.error.empty() ? "" : " (" + r.error + ")") + "; retrying in " +
                           std::to_string(static_cast<int>(wait)) + "s");
    std::this_thread::sleep_for(std::chrono::duration<double>(wait));
  }
}

}  // namespace et::agent
