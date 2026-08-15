#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

// The third layer in this codebase that names a role rather than a mechanism,
// and it exists for the reason the other two do. `ui/` asks `theme()` for a
// colour and `glyphs()` for a character instead of writing one down, because a
// call site that names the mechanism has already chosen it and the second
// answer then needs every call site rewritten rather than the layer extended.
// Everything that reaches outside the process asks here for the same reason:
// `fork()` is a mechanism, "run this and tell me what it printed" is the role,
// and only one of those has a Windows answer.
//
// posix.cpp and windows.cpp are the two implementations and CMakeLists picks
// one, rather than both living behind an `#ifdef` in a shared file. A file that
// compiles on both platforms but only ever runs on one is a file whose dead
// half rots without anyone noticing — which is exactly what happened to the
// nerd glyph set for three releases.
namespace gittop::platform {

// ------------------------------------------------------------------ processes

// Runs `argv` with all three standard streams discarded and returns its exit
// status, or -1 if it could not be run at all.
//
// Never a shell on either platform. Every caller here is assembling a path out
// of the environment or holding a string that came off the network, and the
// quoting rules for /bin/sh and for cmd.exe are two different things to get
// exactly right forever.
int RunQuiet(const std::vector<std::string>& argv);

// Runs `argv` and returns what it wrote to stdout, truncated to `limit` bytes.
// stderr is discarded — a tool complaining about a missing display would
// otherwise land on the alternate screen this program is drawing on.
//
// `exited_ok` distinguishes the two failures that look alike from the outside:
// a tool that is not installed, and one that ran and found nothing. Pass null
// if the difference does not matter.
std::string RunCapture(const std::vector<std::string>& argv, std::size_t limit,
                       bool* exited_ok = nullptr);

// -------------------------------------------------------------------- desktop

// The clipboard's text, or empty. Truncated to `limit` bytes.
//
// This is a role and not a wrapper around one mechanism, because the two
// platforms do not resemble each other here at all: Windows has a clipboard in
// the operating system and answers from the API directly, while X11 and Wayland
// keep it in another *process* and the only portable way in is to run one of
// three helper tools that may or may not be installed.
std::string ReadClipboard(std::size_t limit);

// What to say when that came back empty, which is not the same sentence on both
// platforms. On Windows an empty clipboard is the only explanation there is. On
// POSIX the likelier one by far is that none of the three helper tools is
// installed, and naming one is the difference between a user installing it and
// a user filing this as a gittop bug.
const char* ClipboardEmptyHint();

// Opens `url` in whatever the user's browser is. Returns false if nothing could
// be launched.
//
// The caller has already checked that this is an ordinary https URL — see
// `remote::OpenInBrowser`, which is the only caller and where that check lives,
// since it is a statement about untrusted input rather than about a platform.
// This function is allowed to assume it and would be unsafe without it.
bool OpenUrl(const std::string& url);

// ---------------------------------------------------------------- filesystem

// Restricts `path` — a file or a directory — to its owner and nobody else, the
// way a token's file and its directory have to be.
//
// This is `chmod` 0600/0700 on POSIX and a DACL on Windows, and the two are not
// as similar as they look: a Windows file inherits its parent's permissions
// unless the DACL is explicitly *protected*, so the naive port writes an ACL
// that grants the owner and then leaves everything the parent granted in place
// beside it. A token file that silently ends up readable is worse than one that
// fails loudly, so this reports failure rather than doing its best quietly.
bool RestrictToOwner(const std::string& path, std::string* error = nullptr);

// The running executable's own path. Empty if it cannot be determined.
//
// gittop needs this because ssh runs `SSH_ASKPASS`, and what gittop points that
// at is itself — so it has to be able to name itself in a way that survives
// being exec'd by somebody else, which argv[0] does not.
std::string ExecutablePath();

// The user's home directory, or empty. `$HOME`, or `%USERPROFILE%`.
std::string HomeDir();

// The directory gittop's own configuration belongs in, already joined with
// "gittop": `$XDG_CONFIG_HOME/gittop`, `~/.config/gittop`, or `%APPDATA%\gittop`.
// Empty when there is no home directory to hang it off.
std::string ConfigDir();

// ----------------------------------------------------------------- terminal

// Whether stdout is a terminal rather than a pipe or a file. The splash is
// skipped when it is not, since an animation redrawn into a capture file is
// several thousand frames nobody will read.
bool StdoutIsTerminal();

// How much colour the console this process is attached to can render, in bits:
// 0 for none, or 4, 8, 24. **-1 means this platform has nothing to say** and the
// caller should decide from the environment as it always has.
//
// POSIX always answers -1, and that is not a stub. TERM and COLORTERM are how a
// terminal answers this question there, they are essentially always set, and a
// console has no independent opinion worth preferring over them.
//
// Windows is the reason this exists. A Windows console sets neither variable, so
// the environment has nothing to say and `ui::DetectColorDepth` would read the
// silence as TERM=dumb and go monochrome on a machine rendering 24-bit colour.
// The console's own capabilities are the only source of truth there, and they
// follow the build number: virtual terminal sequences arrived in 10586 and
// 24-bit colour in 15063.
int ConsoleColorBits();

// ----------------------------------------------------------------- time

// Converts a broken-down UTC time to a Unix timestamp — the inverse of gmtime,
// and *not* the same as mktime, which reads the local timezone and would shift
// every server timestamp gittop parses by the machine's offset.
//
// This is here only because the function has two names. POSIX calls it timegm,
// the Windows CRT calls it _mkgmtime, and neither declares the other. That is a
// thin reason for a platform entry point and it is still the right place for
// it: the alternative is an #ifdef in remote/api.cpp, which is a file about
// what a provider sends and has no business knowing which platform it is on.
std::int64_t TimestampFromUtc(std::tm& tm);

// And back: a Unix timestamp broken down into UTC fields. The thread-safe form
// of gmtime, which POSIX spells gmtime_r and the Windows CRT spells gmtime_s —
// with the arguments the other way round, which is why this is a function here
// rather than a macro aliasing one name to the other.
//
// Plain gmtime is not an option on either platform: it returns a pointer to a
// shared static buffer, and gittop draws the graph panel from a worker's
// snapshot while the UI thread is formatting timestamps of its own.
std::tm UtcFromTimestamp(std::int64_t seconds);

// ------------------------------------------------------------- environment

// Writes into *this* process's environment, which is what a child inherits.
//
// gittop needs this at all because libgit2's exec ssh transport offers no way
// to set the child's environment: the only channel to the ssh it spawns is the
// environment of the process that spawns it. That makes these process-global,
// so they are called from the UI thread with no transfer in flight.
//
// On Windows a process has two environments — the CRT's copy and the Win32
// block — and a child inherits the second while getenv reads the first, so the
// implementation there writes both. Setting only one is a bug that looks like
// the variable was never set, from whichever side you did not write.
bool SetEnv(const char* name, const char* value);
void UnsetEnv(const char* name);

// ------------------------------------------------------------------- ssh

// Whether an ssh agent could plausibly be running, asked before paying for a
// process to find out for certain.
//
// POSIX answers from `SSH_AUTH_SOCK`, which is where the agent's address lives.
// Windows has no such variable — its agent is a service on a fixed named pipe
// that ssh-add finds by itself — so there the honest answer is "maybe", and the
// `ssh-add -l` behind this is the only thing that can settle it. Returning
// false there instead would prompt for a passphrase every time, including for
// the people who already solved the problem properly.
bool MaySshAgentBeRunning();

// -------------------------------------------------------------- secret channel

// A one-shot, same-user-only channel for handing a secret to a child process,
// which is how the ssh key passphrase reaches the `ssh` that libgit2 exec'd.
//
// The secret travels over this and never through the environment, the command
// line, or a file. On POSIX `/proc/<pid>/environ` and `/cmdline` are
// unprivileged reads for the same user, on Windows the equivalent is a handful
// of lines with `NtQueryInformationProcess`, and a file would put a private
// key's passphrase on disk on both. A unix socket in a 0700 directory and a
// named pipe with an owner-only DACL are the two mechanisms; that they are
// different is precisely why the caller is not allowed to know which it has.
//
// `endpoint()` is a *name* — a socket path or a pipe name — so unlike the
// secret it is safe to put in the environment, which is how the child is told
// where to look.
class SecretServer {
 public:
  explicit SecretServer(std::string secret);
  ~SecretServer();

  SecretServer(const SecretServer&) = delete;
  SecretServer& operator=(const SecretServer&) = delete;

  // Whether the channel came up at all. A false here is not fatal: it means the
  // passphrase cannot be delivered, so the transfer proceeds without one and
  // fails the way it did before any of this existed.
  bool ok() const;

  // Whether anything ever collected the secret. This is what tells a wrong
  // passphrase apart from an ordinary authentication failure — and a success
  // where nothing ever asked means the passphrase was not what authenticated,
  // so it is dropped rather than kept for a session that does not need it.
  bool served() const;

  const std::string& endpoint() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// The client half, run in the copy of gittop that ssh spawned as its askpass
// helper. Returns the secret, or empty.
std::string ReadSecretFrom(const std::string& endpoint);

}  // namespace gittop::platform
