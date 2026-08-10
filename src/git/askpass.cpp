#include "git/askpass.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace gittop::git {
namespace {

constexpr const char* kSocketEnv = "GITTOP_ASKPASS_SOCKET";

// The passphrase is short, but neither a write nor a read is guaranteed to
// take all of it in one go.

// For the socket. MSG_NOSIGNAL rather than a SIGPIPE handler: an ssh child that
// gives up between connect and read must not take gittop down with it, and
// installing a process-wide signal disposition from a library file would reach
// well beyond what this one is responsible for.
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

// For stdout in the helper, which is a pipe ssh made and not a socket, so
// send() would fail on it with ENOTSOCK.
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

std::string ReadAll(int fd) {
  std::string out;
  char buffer[256];
  while (true) {
    const ssize_t n = ::read(fd, buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      break;
    }
    out.append(buffer, static_cast<std::size_t>(n));
  }
  return out;
}

// Where the runtime socket goes. XDG_RUNTIME_DIR is per-user and usually a
// tmpfs that never reaches disk, which is the right home for this; /tmp is the
// fallback and the directory is 0700 either way.
std::filesystem::path RuntimeBase() {
  const char* xdg = std::getenv("XDG_RUNTIME_DIR");
  if (xdg != nullptr && *xdg != '\0') {
    return std::filesystem::path(xdg);
  }
  return std::filesystem::path("/tmp");
}

// Runs a command with all three standard streams on /dev/null and returns its
// exit status, or -1 if it could not be run at all.
//
// fork/exec rather than std::system because every argument here is a path built
// out of $HOME, and handing those to a shell would mean getting the quoting
// exactly right forever. Nothing between fork and exec allocates.
int RunQuiet(const std::vector<std::string>& args) {
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const std::string& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    return -1;
  }
  if (pid == 0) {
    const int null_fd = ::open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
      ::dup2(null_fd, STDIN_FILENO);
      ::dup2(null_fd, STDOUT_FILENO);
      ::dup2(null_fd, STDERR_FILENO);
      if (null_fd > STDERR_FILENO) {
        ::close(null_fd);
      }
    }
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

// The identity files ssh tries when nothing in ~/.ssh/config says otherwise.
std::vector<std::filesystem::path> DefaultIdentityFiles() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') {
    return {};
  }
  const std::filesystem::path ssh_dir = std::filesystem::path(home) / ".ssh";
  const char* names[] = {"id_ed25519", "id_ecdsa", "id_ecdsa_sk",
                         "id_ed25519_sk", "id_rsa", "id_dsa"};

  std::vector<std::filesystem::path> found;
  for (const char* name : names) {
    std::error_code ec;
    const std::filesystem::path candidate = ssh_dir / name;
    if (std::filesystem::exists(candidate, ec) && !ec) {
      found.push_back(candidate);
    }
  }
  return found;
}

std::string SelfExePath() {
  std::error_code ec;
  const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec) {
    return {};
  }
  return exe.string();
}

}  // namespace

bool IsSshUrl(const std::string& url) {
  if (url.rfind("ssh://", 0) == 0 || url.rfind("git+ssh://", 0) == 0) {
    return true;
  }
  const std::size_t scheme = url.find("://");
  if (scheme != std::string::npos) {
    return false;  // https, git, file — somebody else's transport
  }
  // scp-like: a colon before the first slash, which is what tells
  // git@github.com:user/repo.git apart from a plain relative path.
  const std::size_t colon = url.find(':');
  if (colon == std::string::npos) {
    return false;
  }
  const std::size_t slash = url.find('/');
  return slash == std::string::npos || colon < slash;
}

bool AgentHasIdentities() {
  const char* sock = std::getenv("SSH_AUTH_SOCK");
  if (sock == nullptr || *sock == '\0') {
    return false;
  }
  // `ssh-add -l` exits 0 with identities, 1 with none, 2 when it cannot reach
  // an agent. Only the first means ssh will get in without asking us.
  return RunQuiet({"ssh-add", "-l"}) == 0;
}

bool HasEncryptedDefaultKey() {
  for (const std::filesystem::path& key : DefaultIdentityFiles()) {
    // `ssh-keygen -y` derives the public key, which it can only do after
    // decrypting the private one. With an empty -P it therefore exits zero for
    // an unencrypted key and non-zero for a passphrase-protected one, which is
    // the question being asked here — and it never touches the network or
    // prompts, because -P supplies the passphrase up front.
    if (RunQuiet({"ssh-keygen", "-y", "-P", "", "-f", key.string()}) != 0) {
      return true;
    }
  }
  return false;
}

bool NeedsPassphrase(const std::string& url) {
  // Order matters: the agent check is the one that makes this stay quiet for
  // people who already solved the problem properly, and it is also the cheapest.
  return IsSshUrl(url) && !AgentHasIdentities() && HasEncryptedDefaultKey();
}

struct AskpassServer::Impl {
  std::string passphrase;
  std::filesystem::path dir;
  std::string socket_path;
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
      if ((fds[1].revents & POLLIN) != 0) {
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
      if (SendAll(client, passphrase.data(), passphrase.size())) {
        served.store(true);
      }
      ::close(client);
    }
  }
};

AskpassServer::AskpassServer(std::string passphrase) : impl_(std::make_unique<Impl>()) {
  impl_->passphrase = std::move(passphrase);

  std::string tmpl = (RuntimeBase() / "gittop-XXXXXX").string();
  // mkdtemp creates the directory 0700, which is the permission that matters
  // here — the socket inside it is unreachable to anyone who cannot traverse it.
  if (::mkdtemp(tmpl.data()) == nullptr) {
    return;
  }
  impl_->dir = tmpl;
  impl_->socket_path = (impl_->dir / "askpass.sock").string();

  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  if (impl_->socket_path.size() >= sizeof(addr.sun_path)) {
    return;
  }
  std::memcpy(addr.sun_path, impl_->socket_path.c_str(), impl_->socket_path.size() + 1);

  impl_->listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (impl_->listen_fd < 0) {
    return;
  }
  if (::bind(impl_->listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0 ||
      ::chmod(impl_->socket_path.c_str(), S_IRUSR | S_IWUSR) < 0 ||
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

AskpassServer::~AskpassServer() {
  if (impl_->stop_pipe[1] >= 0) {
    const char byte = 0;
    (void)::write(impl_->stop_pipe[1], &byte, 1);
  }
  if (impl_->thread.joinable()) {
    impl_->thread.join();
  }
  for (int fd : {impl_->stop_pipe[0], impl_->stop_pipe[1], impl_->listen_fd}) {
    if (fd >= 0) {
      ::close(fd);
    }
  }
  // The passphrase outlives this object only if something copied it, and
  // nothing does — but the socket must not outlive the transfer either way.
  if (!impl_->dir.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(impl_->dir, ec);
  }
}

bool AskpassServer::ok() const { return impl_->listen_fd >= 0; }

bool AskpassServer::served() const { return impl_->served.load(); }

const std::string& AskpassServer::socket_path() const { return impl_->socket_path; }

bool InstallAskpassEnv(const AskpassServer& server) {
  if (!server.ok()) {
    return false;
  }
  const std::string exe = SelfExePath();
  if (exe.empty()) {
    return false;
  }
  if (::setenv("SSH_ASKPASS", exe.c_str(), 1) != 0 ||
      ::setenv("SSH_ASKPASS_REQUIRE", "force", 1) != 0 ||
      ::setenv(kSocketEnv, server.socket_path().c_str(), 1) != 0) {
    return false;
  }
  return true;
}

void ClearAskpassEnv() {
  ::unsetenv("SSH_ASKPASS");
  ::unsetenv("SSH_ASKPASS_REQUIRE");
  ::unsetenv(kSocketEnv);
}

bool RunningAsAskpassHelper() {
  const char* sock = std::getenv(kSocketEnv);
  return sock != nullptr && *sock != '\0';
}

int RunAskpassHelper() {
  const char* sock = std::getenv(kSocketEnv);
  if (sock == nullptr || *sock == '\0') {
    return 1;
  }

  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  const std::size_t len = std::strlen(sock);
  if (len >= sizeof(addr.sun_path)) {
    return 1;
  }
  std::memcpy(addr.sun_path, sock, len + 1);

  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return 1;
  }
  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return 1;
  }

  const std::string passphrase = ReadAll(fd);
  ::close(fd);
  if (passphrase.empty()) {
    return 1;
  }

  // ssh reads one line from the helper's stdout and strips the newline. Written
  // with write() rather than std::cout so nothing of it lingers in a stream
  // buffer that some later flush could put somewhere else.
  const std::string line = passphrase + "\n";
  if (!WriteAll(STDOUT_FILENO, line.data(), line.size())) {
    return 1;
  }
  return 0;
}

}  // namespace gittop::git
