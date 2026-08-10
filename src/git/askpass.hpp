#pragma once

#include <memory>
#include <string>

namespace gittop::git {

// The ssh passphrase channel.
//
// With USE_SSH=exec libgit2 does not authenticate an ssh remote itself: it
// builds an `ssh` command line and execs it, and every credential decision
// happens inside that child. libgit2's credential callback is never consulted,
// so a passphrase prompt cannot be answered through git_credential_* — the
// question is not being asked there.
//
// What `ssh` does offer is SSH_ASKPASS: a program it runs when it needs a
// passphrase and cannot use the terminal. That is the only way in, and it is
// the whole of this file. gittop points SSH_ASKPASS at its own binary, and the
// copy of itself that ssh spawns fetches the passphrase and prints it.

// True when this URL will be handled by the ssh transport: an explicit
// ssh://, or the scp-like git@host:path that every hosted provider hands out.
bool IsSshUrl(const std::string& url);

// Whether an agent is reachable and holds at least one identity. When it does,
// gittop must not prompt: ssh will authenticate on its own, and asking for a
// passphrase nobody needs trains people to type one anywhere it is asked for.
bool AgentHasIdentities();

// Whether any of the default ~/.ssh identity files is passphrase-protected.
bool HasEncryptedDefaultKey();

// Whether to prompt before this transfer: an ssh remote, no agent holding a
// usable identity, and a default key that is actually encrypted. All three,
// because prompting when the answer is not needed is its own kind of wrong —
// an unencrypted key with no agent authenticates perfectly well on its own.
//
// This can still be wrong in both directions, since which key ssh picks depends
// on ~/.ssh/config and gittop does not parse it. Neither error is expensive: a
// prompt that was not needed goes unused, and a missing one leaves exactly the
// failure that happened before any of this existed.
bool NeedsPassphrase(const std::string& url);

// Serves one passphrase to the `ssh` children of a single transfer.
//
// The passphrase travels over a unix socket rather than the environment, argv,
// or a temporary file. All three of those are readable by anything running as
// this user — /proc/<pid>/environ and /proc/<pid>/cmdline are not privileged
// reads — and a file would put a private key's passphrase on disk, which is
// exactly what encrypting the key was meant to avoid. The socket lives in a
// 0700 directory, is itself 0600, and is unlinked when the transfer ends.
//
// The listener runs on its own thread because the ssh child asks while the
// transfer worker is blocked inside libgit2 waiting for that same child.
class AskpassServer {
 public:
  // Binds and starts serving `passphrase`. ok() is false if the socket could
  // not be created, in which case the caller should run the transfer without
  // it rather than fail: an agent-backed key still works.
  explicit AskpassServer(std::string passphrase);
  ~AskpassServer();

  AskpassServer(const AskpassServer&) = delete;
  AskpassServer& operator=(const AskpassServer&) = delete;

  bool ok() const;

  // Whether an ssh child actually collected the passphrase. A failed transfer
  // that never asked was not a wrong passphrase, and saying so would send
  // somebody looking in the wrong place.
  bool served() const;

  // The bound path. Public only so InstallAskpassEnv can name it; the socket is
  // reachable to this user regardless, so this leaks nothing the filesystem
  // does not already say.
  const std::string& socket_path() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Sets SSH_ASKPASS, SSH_ASKPASS_REQUIRE and the socket path in gittop's own
// environment, because libgit2's exec transport gives no way to set the child's.
// That makes these process-global, so both calls belong on the UI thread with
// no transfer in flight — which is where they are made.
//
// SSH_ASKPASS_REQUIRE=force is the part that matters: without it ssh prefers
// the terminal, and the terminal is the one place gittop cannot let it have,
// since the TUI is holding it in raw mode on the alternate screen.
bool InstallAskpassEnv(const AskpassServer& server);
void ClearAskpassEnv();

// The other side. Runs when gittop finds itself spawned as an askpass helper,
// which is decided by the environment rather than a flag: ssh chooses the
// child's argv, and it puts its prompt there.
bool RunningAsAskpassHelper();
int RunAskpassHelper();

}  // namespace gittop::git
