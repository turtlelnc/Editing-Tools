#pragma once
// A tiny scripted HTTP/1.1 server for tests. It listens on 127.0.0.1 with a random port, records each
// request, and answers with whatever response was enqueued for it. Only what the agent needs: POST bodies,
// status codes and a raw body (so tests can stream SSE). Curl talks to it like any other server.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace et::test {

class MockServer {
 public:
  struct Request {
    std::string method, path, body;
    std::map<std::string, std::string> headers;  // keys lower-cased
    std::string header(const std::string& name) const {
      std::string k = name;
      for (auto& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      auto it = headers.find(k);
      return it == headers.end() ? "" : it->second;
    }
  };
  struct Response {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
  };

  MockServer() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) throw std::runtime_error("mock server: socket failed");
    int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
      throw std::runtime_error("mock server: bind failed");
    socklen_t len = sizeof addr;
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
      throw std::runtime_error("mock server: getsockname failed");
    port_ = ntohs(addr.sin_port);
    if (::listen(listen_fd_, 8) != 0) throw std::runtime_error("mock server: listen failed");
    thread_ = std::thread([this] { serve(); });
  }

  ~MockServer() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (listen_fd_ >= 0) ::close(listen_fd_);
  }

  MockServer(const MockServer&) = delete;
  MockServer& operator=(const MockServer&) = delete;

  std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port_); }
  std::string url(const std::string& path) const { return base_url() + path; }
  int port() const { return port_; }

  void enqueue(Response r) {
    std::lock_guard<std::mutex> lock(mutex_);
    responses_.push_back(std::move(r));
  }
  void enqueue(std::string body, int status = 200, std::string content_type = "application/json") {
    enqueue(Response{status, std::move(content_type), std::move(body)});
  }

  std::vector<Request> requests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }
  size_t request_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_.size();
  }
  Request request(size_t i) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (i >= requests_.size()) throw std::out_of_range("mock server: no request " + std::to_string(i));
    return requests_[i];
  }
  Request last_request() const { return request(request_count() - 1); }

 private:
  void serve() {
    while (!stop_) {
      pollfd p{listen_fd_, POLLIN, 0};
      int ready = ::poll(&p, 1, 100);
      if (ready <= 0) continue;
      if (stop_) break;
      int fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) continue;
      handle(fd);
      ::close(fd);
    }
  }

  static bool find_header_end(const std::string& buf, size_t& pos) {
    auto at = buf.find("\r\n\r\n");
    if (at == std::string::npos) return false;
    pos = at + 4;
    return true;
  }

  void handle(int fd) {
    std::string buf;
    char tmp[8192];
    size_t header_end = 0;
    Request req;
    bool have_headers = false;
    size_t need = 0;
    while (true) {
      ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
      if (n <= 0) break;
      buf.append(tmp, static_cast<size_t>(n));
      if (!have_headers && find_header_end(buf, header_end)) {
        if (!parse_headers(buf.substr(0, header_end), req)) break;
        have_headers = true;
        if (req.header("expect").find("100-continue") != std::string::npos) {
          const char* cont = "HTTP/1.1 100 Continue\r\n\r\n";
          ::send(fd, cont, std::strlen(cont), 0);
        }
        need = static_cast<size_t>(std::stoul(req.header("content-length").empty() ? "0" : req.header("content-length")));
      }
      if (have_headers && buf.size() >= header_end + need) {
        req.body = buf.substr(header_end, need);
        break;
      }
    }
    if (!have_headers) return;
    Response resp;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      requests_.push_back(req);
      if (!responses_.empty()) {
        resp = responses_.front();
        responses_.erase(responses_.begin());
      } else {
        resp = Response{500, "text/plain", "mock server: no scripted response"};
      }
    }
    std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " " + status_text(resp.status) + "\r\n" +
                      "Content-Type: " + resp.content_type + "\r\n" + "Content-Length: " + std::to_string(resp.body.size()) +
                      "\r\n" + "Connection: close\r\n\r\n" + resp.body;
    size_t off = 0;
    while (off < out.size()) {
      ssize_t n = ::send(fd, out.data() + off, out.size() - off, 0);
      if (n <= 0) break;
      off += static_cast<size_t>(n);
    }
  }

  static bool parse_headers(const std::string& head, Request& req) {
    size_t line_end = head.find("\r\n");
    if (line_end == std::string::npos) return false;
    const std::string first = head.substr(0, line_end);
    size_t sp1 = first.find(' ');
    size_t sp2 = first.find(' ', sp1 == std::string::npos ? sp1 : sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
    req.method = first.substr(0, sp1);
    req.path = first.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t pos = line_end + 2;
    while (pos < head.size()) {
      size_t next = head.find("\r\n", pos);
      if (next == std::string::npos) next = head.size();
      const std::string line = head.substr(pos, next - pos);
      pos = next + 2;
      const size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string name = line.substr(0, colon), value = line.substr(colon + 1);
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
      while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
      for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      req.headers[name] = value;
    }
    return true;
  }

  static const char* status_text(int code) {
    switch (code) {
      case 200: return "OK";
      case 401: return "Unauthorized";
      case 403: return "Forbidden";
      case 404: return "Not Found";
      case 429: return "Too Many Requests";
      case 500: return "Internal Server Error";
      case 502: return "Bad Gateway";
      case 503: return "Service Unavailable";
      default: return "Status";
    }
  }

  int listen_fd_ = -1;
  int port_ = 0;
  std::thread thread_;
  mutable std::mutex mutex_;
  std::vector<Request> requests_;
  std::vector<Response> responses_;
  std::atomic<bool> stop_{false};
};

}  // namespace et::test
