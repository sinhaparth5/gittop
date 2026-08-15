#include "git/askpass.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "platform/platform.hpp"

namespace gittop::git {
namespace {

constexpr const char* kEndpointEnv = "GITTOP_ASKPASS_SOCKET";

// The identity files ssh tries when nothing in ~/.ssh/config says otherwise.
//
// ~/.ssh is the same directory on both platforms — Windows OpenSSH reads
// %USERPROFILE%\.ssh and uses the same file names — so only the home directory
// is platform-dependent, and platform::HomeDir already knows the three places
// it can be written down here.
std::vector<std::filesystem::path> DefaultIdentityFiles() {
  const std::string home = platform::HomeDir();
  if (home.empty()) {
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
  // The cheap check first, and on Windows there is no cheap check to make —
  // see platform::MaySshAgentBeRunning, where the agent is a service on a fixed
  // pipe rather than an address in the environment.
  if (!platform::MaySshAgentBeRunning()) {
    return false;
  }
  // `ssh-add -l` exits 0 with identities, 1 with none, 2 when it cannot reach
  // an agent. Only the first means ssh will get in without asking us.
  return platform::RunQuiet({"ssh-add", "-l"}) == 0;
}

bool HasEncryptedDefaultKey() {
  for (const std::filesystem::path& key : DefaultIdentityFiles()) {
    // `ssh-keygen -y` derives the public key, which it can only do after
    // decrypting the private one. With an empty -P it therefore exits zero for
    // an unencrypted key and non-zero for a passphrase-protected one, which is
    // the question being asked here — and it never touches the network or
    // prompts, because -P supplies the passphrase up front.
    if (platform::RunQuiet({"ssh-keygen", "-y", "-P", "", "-f", key.string()}) != 0) {
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
  platform::SecretServer channel;
  explicit Impl(std::string passphrase) : channel(std::move(passphrase)) {}
};

AskpassServer::AskpassServer(std::string passphrase)
    : impl_(std::make_unique<Impl>(std::move(passphrase))) {}

AskpassServer::~AskpassServer() = default;

bool AskpassServer::ok() const { return impl_->channel.ok(); }

bool AskpassServer::served() const { return impl_->channel.served(); }

const std::string& AskpassServer::endpoint() const { return impl_->channel.endpoint(); }

bool InstallAskpassEnv(const AskpassServer& server) {
  if (!server.ok()) {
    return false;
  }
  const std::string exe = platform::ExecutablePath();
  if (exe.empty()) {
    return false;
  }
  return platform::SetEnv("SSH_ASKPASS", exe.c_str()) &&
         platform::SetEnv("SSH_ASKPASS_REQUIRE", "force") &&
         platform::SetEnv(kEndpointEnv, server.endpoint().c_str());
}

void ClearAskpassEnv() {
  platform::UnsetEnv("SSH_ASKPASS");
  platform::UnsetEnv("SSH_ASKPASS_REQUIRE");
  platform::UnsetEnv(kEndpointEnv);
}

bool RunningAsAskpassHelper() {
  const char* endpoint = std::getenv(kEndpointEnv);
  return endpoint != nullptr && *endpoint != '\0';
}

int RunAskpassHelper() {
  const char* endpoint = std::getenv(kEndpointEnv);
  if (endpoint == nullptr || *endpoint == '\0') {
    return 1;
  }

  const std::string passphrase = platform::ReadSecretFrom(endpoint);
  if (passphrase.empty()) {
    return 1;
  }

  // ssh reads one line from the helper's stdout and strips the newline.
  //
  // Written with fwrite to the C stream rather than std::cout because this
  // process is a one-shot helper whose entire output is a secret: an iostream
  // would leave a copy in its own buffer as well as this one, and there is
  // nothing to gain from formatting. The explicit fflush is what makes the
  // write happen before the return below rather than at some later teardown.
  const std::string line = passphrase + "\n";
  if (std::fwrite(line.data(), 1, line.size(), stdout) != line.size()) {
    return 1;
  }
  if (std::fflush(stdout) != 0) {
    return 1;
  }
  return 0;
}

}  // namespace gittop::git
