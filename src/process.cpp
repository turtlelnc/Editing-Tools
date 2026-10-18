#include "process.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace et {

ProcResult run(const std::vector<std::string>& argv) {
  if (argv.empty()) throw std::invalid_argument("empty argv");
  int out_pipe[2], err_pipe[2];
  if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) throw std::runtime_error("pipe failed");

  pid_t pid = fork();
  if (pid < 0) throw std::runtime_error("fork failed");
  if (pid == 0) {
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);
    close(out_pipe[0]); close(out_pipe[1]);
    close(err_pipe[0]); close(err_pipe[1]);
    int devnull = open("/dev/null", 0);
    if (devnull >= 0) dup2(devnull, STDIN_FILENO);
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    execvp(args[0], args.data());
    _exit(127);
  }
  close(out_pipe[1]);
  close(err_pipe[1]);

  ProcResult r;
  pollfd fds[2] = {{out_pipe[0], POLLIN, 0}, {err_pipe[0], POLLIN, 0}};
  std::string* sinks[2] = {&r.out, &r.err};
  int open_fds = 2;
  char buf[65536];
  while (open_fds > 0) {
    if (poll(fds, 2, -1) < 0) break;
    for (int i = 0; i < 2; ++i) {
      if (fds[i].fd < 0) continue;
      if (fds[i].revents & (POLLIN | POLLHUP)) {
        ssize_t n = read(fds[i].fd, buf, sizeof buf);
        if (n > 0) sinks[i]->append(buf, n);
        else { close(fds[i].fd); fds[i].fd = -1; --open_fds; }
      }
    }
  }
  int status = 0;
  waitpid(pid, &status, 0);
  r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return r;
}

std::string ffmpeg_bin() {
  const char* e = std::getenv("EDITING_TOOLS_FFMPEG");
  return (e && *e) ? e : "ffmpeg";
}

bool ffmpeg_has_filter(const std::string& name) {
  static std::mutex mu;
  static std::map<std::string, bool> cache;  // keyed by binary + filter
  std::lock_guard<std::mutex> lock(mu);
  const std::string key = ffmpeg_bin() + "\n" + name;
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  bool found = false;
  auto r = run({ffmpeg_bin(), "-hide_banner", "-filters"});
  if (r.exit_code == 0) {
    std::istringstream lines(r.out);
    std::string line;
    while (std::getline(lines, line)) {
      std::istringstream f(line);
      std::string flags, fname;
      if ((f >> flags >> fname) && fname == name) { found = true; break; }
    }
  }
  return cache[key] = found;
}

}  // namespace et
