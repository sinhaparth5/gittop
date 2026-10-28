#include "platform/platform.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#if defined(__linux__)
#include <sys/inotify.h>
#endif
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <thread>
#include <utility>
#include <vector>

namespace gittop::platform {
namespace {

std::string Env(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string() : std::string(value);
}

// Nothing between fork and exec allocates, which is why the argv is built
// before the fork rather than inside the child.
std::vector<char*> BuildArgv(const std::vector<std::string>& args) {
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const std::string& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);
  return argv;
}

void RedirectToNull(int flags, std::initializer_list<int> targets) {
  const int null_fd = ::open("/dev/null", flags);
  if (null_fd < 0) {
    return;
  }
  for (const int target : targets) {
    ::dup2(null_fd, target);
  }
  if (null_fd > STDERR_FILENO) {
    ::close(null_fd);
  }
}

// MSG_NOSIGNAL rather than a SIGPIPE handler: an ssh child that gives up
// between connect and read must not take gittop down with it, and installing a
// process-wide signal disposition from a file this far down would reach well
// beyond what it is responsible for.
bool SendAll(int fd, const char* data, std::size_t size) {
  while (size > 0) {
    const ssize_t n = ::send(fd, data, size, MSG_NOSIGNAL);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) {
        continue;
      }
      return false;
    }
    data += n;
    size -= static_cast<std::size_t>(n);
  }
  return true;
}

// For a pipe rather than a socket, where send() would fail with ENOTSOCK.
bool WriteAll(int fd, const char* data, std::size_t size) {
  while (size > 0) {
    const ssize_t n = ::write(fd, data, size);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) {
        continue;
      }
      return false;
    }
    data += n;
    size -= static_cast<std::size_t>(n);
  }
  return true;
}

std::string ReadAll(int fd, std::size_t limit) {
  std::string out;
  char buffer[4096];
  // Drained to the end rather than abandoned at the limit: closing the pipe on
  // a child with more to say kills it with SIGPIPE, which reads as a failure.
  while (true) {
    const ssize_t n = ::read(fd, buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      break;
    }
    if (out.size() < limit) {
      out.append(buffer, static_cast<std::size_t>(n));
    }
  }
  if (out.size() > limit) {
    out.resize(limit);
  }
  return out;
}

// Where the runtime socket goes. XDG_RUNTIME_DIR is per-user and usually a
// tmpfs that never reaches disk, which is the right home for this; /tmp is the
// fallback and the directory is 0700 either way.
std::filesystem::path RuntimeBase() {
  const std::string xdg = Env("XDG_RUNTIME_DIR");
  if (!xdg.empty()) {
    return std::filesystem::path(xdg);
  }
  return std::filesystem::path("/tmp");
}

}  // namespace

// ------------------------------------------------------------------ processes

int RunQuiet(const std::vector<std::string>& args) {
  if (args.empty()) {
    return -1;
  }
  std::vector<char*> argv = BuildArgv(args);

  const pid_t pid = ::fork();
  if (pid < 0) {
    return -1;
  }
  if (pid == 0) {
    RedirectToNull(O_RDWR, {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO});
    ::execvp(argv[0], argv.data());
    _exit(127);
  }

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return -1;
    }
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int RunInTerminal(const std::vector<std::string>& args) {
  if (args.empty()) {
    return -1;
  }
  std::vector<char*> argv = BuildArgv(args);

  const pid_t pid = ::fork();
  if (pid < 0) {
    return -1;
  }
  if (pid == 0) {
    ::execvp(argv[0], argv.data());
    _exit(127);
  }

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return -1;
    }
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string RunCapture(const std::vector<std::string>& args, std::size_t limit,
                       bool* exited_ok) {
  if (exited_ok != nullptr) {
    *exited_ok = false;
  }
  if (args.empty()) {
    return {};
  }

  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) {
    return {};
  }

  std::vector<char*> argv = BuildArgv(args);
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(fds[0]);
    ::close(fds[1]);
    return {};
  }

  if (pid == 0) {
    ::dup2(fds[1], STDOUT_FILENO);
    ::close(fds[0]);
    ::close(fds[1]);
    RedirectToNull(O_WRONLY, {STDERR_FILENO});
    ::execvp(argv[0], argv.data());
    _exit(127);
  }

  ::close(fds[1]);
  std::string out = ReadAll(fds[0], limit);
  ::close(fds[0]);

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return out;
    }
  }
  if (exited_ok != nullptr) {
    *exited_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }
  return out;
}

std::string RunCommand(const std::vector<std::string>& args, const std::string& cwd,
                       std::size_t limit, int* status) {
  *status = -1;
  if (args.empty()) {
    return {};
  }
  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) {
    return {};
  }

  std::vector<char*> argv = BuildArgv(args);
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(fds[0]);
    ::close(fds[1]);
    return {};
  }

  if (pid == 0) {
    // A session of its own has no controlling terminal, so ssh cannot open
    // /dev/tty to ask for a passphrase; GIT_TERMINAL_PROMPT stops git asking
    // for an https password the same way.
    ::setsid();
    ::setenv("GIT_TERMINAL_PROMPT", "0", 1);
    ::dup2(fds[1], STDOUT_FILENO);
    ::dup2(fds[1], STDERR_FILENO);
    ::close(fds[0]);
    ::close(fds[1]);
    RedirectToNull(O_RDONLY, {STDIN_FILENO});
    if (::chdir(cwd.c_str()) != 0) {
      _exit(127);
    }
    ::execvp(argv[0], argv.data());
    _exit(127);
  }

  ::close(fds[1]);
  std::string out = ReadAll(fds[0], limit);
  ::close(fds[0]);

  int wait_status = 0;
  while (::waitpid(pid, &wait_status, 0) < 0) {
    if (errno != EINTR) {
      return out;
    }
  }
  *status = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1;
  return out;
}

// -------------------------------------------------------------------- desktop

std::string ReadClipboard(std::size_t limit) {
  // Wayland first, then the two X11 tools, which is the order they are most
  // likely to be the right one in. None of them is guaranteed to be installed,
  // and that is the ordinary case for two of the three.
  static const char* const kTools[][4] = {
      {"wl-paste", "--no-newline", nullptr, nullptr},
      {"xclip", "-selection", "clipboard", "-o"},
      {"xsel", "--clipboard", "--output", nullptr},
  };

  for (const auto& tool : kTools) {
    std::vector<std::string> args;
    for (const char* arg : tool) {
      if (arg != nullptr) {
        args.emplace_back(arg);
      }
    }
    bool ran = false;
    std::string out = RunCapture(args, limit, &ran);
    if (ran && !out.empty()) {
      return out;
    }
    // A tool that is present but exited non-zero has answered: the clipboard is
    // empty, or this is the wrong display server. Trying the next one costs a
    // fork and settles which of those it was.
  }
  return {};
}

const char* ClipboardEmptyHint() {
  return "clipboard is empty, or needs wl-clipboard / xclip installed";
}

bool OpenUrl(const std::string& url) {
  static constexpr const char* kOpeners[] = {
      "xdg-open",  // freedesktop
      "wslview",   // WSL, where xdg-open often exists and does nothing useful
      "open",      // macOS
  };

  // Double fork, so the opener is reparented to init and never becomes a
  // zombie: there is no later point in a TUI's life that would naturally reap
  // it, and a dashboard that leaks a process per sign-in is a dashboard with a
  // slow leak.
  const pid_t first = ::fork();
  if (first < 0) {
    return false;
  }
  if (first == 0) {
    if (::fork() == 0) {
      // The opener writes to stderr on a good day and to stdout on a bad one,
      // and both of those are the alternate screen this program is drawing on.
      RedirectToNull(O_RDWR, {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO});
      ::setsid();
      for (const char* opener : kOpeners) {
        ::execlp(opener, opener, url.c_str(), static_cast<char*>(nullptr));
      }
      _exit(127);
    }
    _exit(0);
  }

  int status = 0;
  ::waitpid(first, &status, 0);
  return true;
}

// ---------------------------------------------------------------- filesystem

bool RestrictToOwner(const std::string& path, std::string* error) {
  std::error_code ec;
  const bool is_dir = std::filesystem::is_directory(path, ec);
  const mode_t mode = is_dir ? S_IRWXU : (S_IRUSR | S_IWUSR);
  if (::chmod(path.c_str(), mode) != 0) {
    if (error != nullptr) {
      *error = "could not restrict " + path + " to its owner: " + std::strerror(errno);
    }
    return false;
  }
  return true;
}

std::string ExecutablePath() {
  std::error_code ec;
  const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec) {
    return {};
  }
  return exe.string();
}

std::string HomeDir() { return Env("HOME"); }

std::string ConfigDir() {
  // XDG first: a user who has set it has said where this goes.
  const std::string xdg = Env("XDG_CONFIG_HOME");
  if (!xdg.empty()) {
    return (std::filesystem::path(xdg) / "gittop").string();
  }
  const std::string home = HomeDir();
  if (home.empty()) {
    return {};
  }
  return (std::filesystem::path(home) / ".config" / "gittop").string();
}

// ----------------------------------------------------------------- terminal

bool StdoutIsTerminal() { return ::isatty(STDOUT_FILENO) == 1; }

int ConsoleColorBits() { return -1; }  // TERM and COLORTERM already answer this

// ----------------------------------------------------------------- time

std::int64_t TimestampFromUtc(std::tm& tm) {
  return static_cast<std::int64_t>(::timegm(&tm));
}

std::tm UtcFromTimestamp(std::int64_t seconds) {
  const auto value = static_cast<std::time_t>(seconds);
  std::tm out{};
  ::gmtime_r(&value, &out);
  return out;
}

// ------------------------------------------------------------- environment

bool SetEnv(const char* name, const char* value) {
  return ::setenv(name, value, 1) == 0;
}

void UnsetEnv(const char* name) { ::unsetenv(name); }

// ------------------------------------------------------------------- ssh

bool MaySshAgentBeRunning() { return !Env("SSH_AUTH_SOCK").empty(); }

// -------------------------------------------------------------- secret channel

struct SecretServer::Impl {
  std::string secret;
  std::filesystem::path dir;
  std::string endpoint;
  int listen_fd = -1;
  int stop_pipe[2] = {-1, -1};
  std::thread thread;
  std::atomic<bool> served{false};

  void Serve() {
    while (true) {
      struct pollfd fds[2];
      fds[0] = {listen_fd, POLLIN, 0};
      fds[1] = {stop_pipe[0], POLLIN, 0};

      const int ready = ::poll(fds, 2, -1);
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        return;
      }
      // POLLHUP as well as POLLIN: the destructor closes the write end, and a
      // byte that never arrived leaves the hangup as the only signal there is.
      // Checking POLLIN alone would spin here instead — poll returns
      // immediately on a hungup pipe, every time, forever.
      if ((fds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        return;  // the transfer ended
      }
      if ((fds[0].revents & POLLIN) == 0) {
        continue;
      }

      const int client = ::accept(listen_fd, nullptr, nullptr);
      if (client < 0) {
        continue;
      }
      // Filesystem permissions are the whole access check, exactly as they are
      // for ssh-agent's own socket: the directory is 0700, so a peer that got
      // this far is already running as this user.
      if (SendAll(client, secret.data(), secret.size())) {
        served.store(true);
      }
      ::close(client);
    }
  }
};

SecretServer::SecretServer(std::string secret) : impl_(std::make_unique<Impl>()) {
  impl_->secret = std::move(secret);

  std::string tmpl = (RuntimeBase() / "gittop-XXXXXX").string();
  // mkdtemp creates the directory 0700, which is the permission that matters
  // here — the socket inside it is unreachable to anyone who cannot traverse it.
  if (::mkdtemp(tmpl.data()) == nullptr) {
    return;
  }
  impl_->dir = tmpl;
  impl_->endpoint = (impl_->dir / "askpass.sock").string();

  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  if (impl_->endpoint.size() >= sizeof(addr.sun_path)) {
    return;
  }
  std::memcpy(addr.sun_path, impl_->endpoint.c_str(), impl_->endpoint.size() + 1);

  impl_->listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (impl_->listen_fd < 0) {
    return;
  }
  if (::bind(impl_->listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0 ||
      ::chmod(impl_->endpoint.c_str(), S_IRUSR | S_IWUSR) < 0 ||
      ::listen(impl_->listen_fd, 4) < 0) {
    ::close(impl_->listen_fd);
    impl_->listen_fd = -1;
    return;
  }
  if (::pipe(impl_->stop_pipe) < 0) {
    ::close(impl_->listen_fd);
    impl_->listen_fd = -1;
    return;
  }

  impl_->thread = std::thread([this] { impl_->Serve(); });
}

SecretServer::~SecretServer() {
  // This byte is the only thing that ends Serve's poll, and the join() below
  // waits on that thread forever — so a write that does not land hangs the quit
  // path rather than losing a notification nobody reads. A bare ::write can fail
  // with EINTR, which is why this goes through WriteAll and its retry like every
  // other write in this file.
  //
  // Closing the write end here rather than with the other fds after the join is
  // the belt to that braces: an empty pipe with no writer left polls POLLHUP, so
  // the thread wakes even in the case where the write failed anyway.
  if (impl_->stop_pipe[1] >= 0) {
    const char byte = 0;
    (void)WriteAll(impl_->stop_pipe[1], &byte, 1);
    ::close(impl_->stop_pipe[1]);
    impl_->stop_pipe[1] = -1;
  }
  if (impl_->thread.joinable()) {
    impl_->thread.join();
  }
  for (int fd : {impl_->stop_pipe[0], impl_->stop_pipe[1], impl_->listen_fd}) {
    if (fd >= 0) {
      ::close(fd);
    }
  }
  // The secret outlives this object only if something copied it, and nothing
  // does — but the socket must not outlive the transfer either way.
  if (!impl_->dir.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(impl_->dir, ec);
  }
}

bool SecretServer::ok() const { return impl_->listen_fd >= 0; }

bool SecretServer::served() const { return impl_->served.load(); }

const std::string& SecretServer::endpoint() const { return impl_->endpoint; }

std::string ReadSecretFrom(const std::string& endpoint) {
  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  if (endpoint.empty() || endpoint.size() >= sizeof(addr.sun_path)) {
    return {};
  }
  std::memcpy(addr.sun_path, endpoint.c_str(), endpoint.size() + 1);

  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return {};
  }
  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return {};
  }

  // A passphrase is short, but a read is not guaranteed to take all of it in
  // one go. The limit is generous and only there so a channel that never closes
  // cannot grow this without bound.
  std::string secret = ReadAll(fd, 64 * 1024);
  ::close(fd);
  return secret;
}

struct TreeWatcher::Impl {
  std::string root;
  std::function<bool(const std::string&)> skip;
  std::function<void()> changed;
  int fd = -1;
  int stop_pipe[2] = {-1, -1};
  std::thread thread;

#if defined(__linux__)
  // inotify is not recursive, so every directory is a watch of its own and a
  // new one has to be added as it appears. This maps each back to its path.
  std::unordered_map<int, std::string> dirs;

  void Add(const std::string& rel) {
    const std::string full = rel.empty() ? root : root + "/" + rel;
    const int wd = ::inotify_add_watch(fd, full.c_str(),
                                       IN_CREATE | IN_DELETE | IN_CLOSE_WRITE | IN_MOVED_FROM |
                                           IN_MOVED_TO | IN_ONLYDIR | IN_DONT_FOLLOW);
    if (wd < 0) {
      return;  // out of watches, or gone already: the rest still work
    }
    dirs[wd] = rel;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(full, ec)) {
      if (entry.is_symlink(ec) || !entry.is_directory(ec)) {
        continue;
      }
      const std::string child = (rel.empty() ? "" : rel + "/") + entry.path().filename().string();
      if (!skip(child + "/")) {
        Add(child);
      }
    }
  }

  void Run() {
    using Clock = std::chrono::steady_clock;
    bool pending = false;
    Clock::time_point due;
    alignas(struct inotify_event) char buffer[16384];
    while (true) {
      int timeout = -1;
      if (pending) {
        timeout = static_cast<int>(std::max<std::int64_t>(
            0, std::chrono::duration_cast<std::chrono::milliseconds>(due - Clock::now()).count()));
      }
      struct pollfd fds[2];
      fds[0] = {fd, POLLIN, 0};
      fds[1] = {stop_pipe[0], POLLIN, 0};
      const int ready = ::poll(fds, 2, timeout);
      if (ready < 0 && errno != EINTR) {
        return;
      }
      // POLLHUP for the reason SecretServer's loop gives.
      if ((fds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        return;
      }
      if (pending && Clock::now() >= due) {
        pending = false;
        changed();
      }
      if (ready <= 0 || (fds[0].revents & POLLIN) == 0) {
        continue;
      }
      const ssize_t got = ::read(fd, buffer, sizeof(buffer));
      bool relevant = false;
      for (ssize_t at = 0; at < got;) {
        const auto* event = reinterpret_cast<const struct inotify_event*>(buffer + at);
        at += static_cast<ssize_t>(sizeof(struct inotify_event) + event->len);
        if ((event->mask & IN_Q_OVERFLOW) != 0) {
          relevant = true;  // events were lost, so assume the worst
          continue;
        }
        if ((event->mask & IN_IGNORED) != 0) {
          dirs.erase(event->wd);
          continue;
        }
        const auto dir = dirs.find(event->wd);
        if (dir == dirs.end() || event->len == 0) {
          continue;
        }
        const std::string rel = (dir->second.empty() ? "" : dir->second + "/") + event->name;
        const bool is_dir = (event->mask & IN_ISDIR) != 0;
        if (skip(is_dir ? rel + "/" : rel)) {
          continue;
        }
        if (is_dir && (event->mask & (IN_CREATE | IN_MOVED_TO)) != 0) {
          Add(rel);
        }
        relevant = true;
      }
      // Due a fixed time after the first change rather than after the last, so
      // something that never stops writing still refreshes, just not often.
      if (relevant && !pending) {
        pending = true;
        due = Clock::now() + std::chrono::milliseconds(kWatchSettleMs);
      }
    }
  }
#endif
};

TreeWatcher::TreeWatcher(std::string root, std::function<bool(const std::string&)> skip,
                         std::function<void()> changed)
    : impl_(std::make_unique<Impl>()) {
  impl_->root = std::move(root);
  while (impl_->root.size() > 1 && impl_->root.back() == '/') {
    impl_->root.pop_back();
  }
  impl_->skip = std::move(skip);
  impl_->changed = std::move(changed);
#if defined(__linux__)
  impl_->fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (impl_->fd < 0) {
    return;
  }
  if (::pipe(impl_->stop_pipe) < 0) {
    ::close(impl_->fd);
    impl_->fd = -1;
    return;
  }
  impl_->Add("");
  impl_->thread = std::thread([this] { impl_->Run(); });
#endif
}

TreeWatcher::~TreeWatcher() {
  // The byte is what ends Run's poll; closing the write end makes it POLLHUP
  // even if the write failed, so the join below cannot hang.
  if (impl_->stop_pipe[1] >= 0) {
    const char byte = 0;
    (void)WriteAll(impl_->stop_pipe[1], &byte, 1);
    ::close(impl_->stop_pipe[1]);
  }
  if (impl_->thread.joinable()) {
    impl_->thread.join();
  }
  for (const int fd : {impl_->fd, impl_->stop_pipe[0]}) {
    if (fd >= 0) {
      ::close(fd);
    }
  }
}

bool TreeWatcher::ok() const { return impl_->thread.joinable(); }

}  // namespace gittop::platform
