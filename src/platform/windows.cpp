#include "platform/platform.hpp"

// Order matters: windows.h has to come first, and NOMINMAX/WIN32_LEAN_AND_MEAN
// are set on the target rather than here so every translation unit that pulls
// windows.h in transitively gets the same one. Without NOMINMAX the min/max
// macros collide with <algorithm> in half this codebase.
#include <windows.h>

#include <aclapi.h>
#include <io.h>
#include <shellapi.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace gittop::platform {
namespace {

// ------------------------------------------------------------------ encoding

// Everything above this layer is UTF-8 — the config, the glyph tokens, the
// commit messages libgit2 hands over — and every Win32 call that takes a string
// is used in its W form. The A forms would go through the process code page,
// which is not UTF-8 on most machines, so a repository path with a non-ASCII
// character in it would fail to open for reasons nothing on screen could
// explain.
std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) {
    return {};
  }
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(),
                        needed);
  return out;
}

std::string Narrow(const std::wstring& wide) {
  if (wide.empty()) {
    return {};
  }
  const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                           static_cast<int>(wide.size()), nullptr, 0,
                                           nullptr, nullptr);
  if (needed <= 0) {
    return {};
  }
  std::string out(static_cast<std::size_t>(needed), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(),
                        needed, nullptr, nullptr);
  return out;
}

std::string Env(const char* name) {
  const std::wstring wname = Widen(name);
  const DWORD needed = ::GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
  if (needed == 0) {
    return {};
  }
  std::wstring value(needed, L'\0');
  const DWORD got = ::GetEnvironmentVariableW(wname.c_str(), value.data(), needed);
  if (got == 0 || got >= needed) {
    return {};
  }
  value.resize(got);
  return Narrow(value);
}

std::string LastErrorText(DWORD code) {
  LPWSTR buffer = nullptr;
  const DWORD length = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  if (length == 0 || buffer == nullptr) {
    return "error " + std::to_string(code);
  }
  std::wstring text(buffer, length);
  ::LocalFree(buffer);
  while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) {
    text.pop_back();
  }
  return Narrow(text);
}

// ------------------------------------------------------------------- argv

// CreateProcess takes one string where execvp takes a vector, so the argument
// boundaries have to be encoded and then decoded again by the child. This is
// the *encode* half of the rule the C runtime documents for its own decoder,
// and getting it wrong is not a cosmetic bug: an unquoted argument holding a
// space silently becomes two, which for a path out of the environment means
// running the wrong thing.
//
// The backslash rule is the part nobody expects. A run of backslashes is
// literal on its own but escapes a following quote, so the run has to be
// doubled when it is about to meet one — which happens before an embedded quote
// and again at the closing quote of the whole argument.
std::wstring QuoteArgument(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    return arg;
  }

  std::wstring out;
  out.push_back(L'"');
  for (auto it = arg.begin();; ++it) {
    std::size_t backslashes = 0;
    while (it != arg.end() && *it == L'\\') {
      ++it;
      ++backslashes;
    }

    if (it == arg.end()) {
      out.append(backslashes * 2, L'\\');
      break;
    }
    if (*it == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
    } else {
      out.append(backslashes, L'\\');
    }
    out.push_back(*it);
  }
  out.push_back(L'"');
  return out;
}

std::wstring BuildCommandLine(const std::vector<std::string>& args) {
  std::wstring line;
  for (const std::string& arg : args) {
    if (!line.empty()) {
      line.push_back(L' ');
    }
    line += QuoteArgument(Widen(arg));
  }
  return line;
}

// The NUL device, which is what /dev/null is called here. Opened inheritable so
// the child gets it.
HANDLE OpenNullDevice(DWORD access) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  return ::CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                       OPEN_EXISTING, 0, nullptr);
}

// Runs `args` with stdin and stderr on NUL. `capture` decides whether stdout is
// a pipe read back into `out` or also NUL. Returns the exit code, or -1 if the
// process could not be started at all — which is the ordinary answer for a tool
// that is not installed, and the difference the callers care about.
int RunProcess(const std::vector<std::string>& args, bool capture, std::size_t limit,
               std::string* out) {
  if (args.empty()) {
    return -1;
  }

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (capture) {
    if (::CreatePipe(&read_end, &write_end, &sa, 0) == 0) {
      return -1;
    }
    // The read end must not reach the child, or the pipe never reports EOF:
    // the child would hold a handle to it and the read below would block after
    // the process exited, waiting for a writer that is this process.
    ::SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
  }

  const HANDLE null_in = OpenNullDevice(GENERIC_READ);
  const HANDLE null_out = OpenNullDevice(GENERIC_WRITE);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = null_in;
  si.hStdOutput = capture ? write_end : null_out;
  si.hStdError = null_out;

  PROCESS_INFORMATION pi{};
  std::wstring command = BuildCommandLine(args);

  // CREATE_NO_WINDOW so a console helper does not flash a window over the
  // dashboard, and does not attach itself to this console and scribble on the
  // alternate screen.
  //
  // lpApplicationName is null so the executable is looked up on PATH and .exe
  // is appended when it has no extension, which is what execvp does and what
  // every caller here assumes.
  const BOOL started =
      ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &si, &pi);

  if (write_end != nullptr) {
    ::CloseHandle(write_end);  // before the read, or EOF never arrives
  }
  if (null_in != INVALID_HANDLE_VALUE) {
    ::CloseHandle(null_in);
  }
  if (null_out != INVALID_HANDLE_VALUE) {
    ::CloseHandle(null_out);
  }

  if (started == 0) {
    if (read_end != nullptr) {
      ::CloseHandle(read_end);
    }
    return -1;
  }

  if (capture && out != nullptr) {
    char buffer[4096];
    DWORD got = 0;
    while (out->size() < limit &&
           ::ReadFile(read_end, buffer, sizeof(buffer), &got, nullptr) != 0 && got > 0) {
      out->append(buffer, got);
    }
    if (out->size() > limit) {
      out->resize(limit);
    }
  }
  if (read_end != nullptr) {
    ::CloseHandle(read_end);
  }

  ::WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = static_cast<DWORD>(-1);
  ::GetExitCodeProcess(pi.hProcess, &code);
  ::CloseHandle(pi.hProcess);
  ::CloseHandle(pi.hThread);
  return static_cast<int>(code);
}

// ------------------------------------------------------------------- security

// An ACL granting the current user and nobody else, which is this platform's
// answer to 0600 and 0700.
//
// The caller owns the returned ACL and frees it with LocalFree. Null on
// failure, and the callers treat that as failure rather than carrying on — an
// unrestricted token file is the outcome this whole function exists to prevent.
PACL OwnerOnlyDacl(DWORD access, bool inheritable) {
  HANDLE token = nullptr;
  if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == 0) {
    return nullptr;
  }

  DWORD size = 0;
  ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  if (size == 0) {
    ::CloseHandle(token);
    return nullptr;
  }
  std::vector<unsigned char> buffer(size);
  if (::GetTokenInformation(token, TokenUser, buffer.data(), size, &size) == 0) {
    ::CloseHandle(token);
    return nullptr;
  }
  ::CloseHandle(token);

  auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());

  EXPLICIT_ACCESS_W entry{};
  entry.grfAccessPermissions = access;
  entry.grfAccessMode = SET_ACCESS;
  entry.grfInheritance =
      inheritable ? (SUB_CONTAINERS_AND_OBJECTS_INHERIT) : NO_INHERITANCE;
  entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
  entry.Trustee.ptstrName = static_cast<LPWSTR>(user->User.Sid);

  PACL acl = nullptr;
  // A null old-ACL is what makes this a replacement rather than an addition.
  if (::SetEntriesInAclW(1, &entry, nullptr, &acl) != ERROR_SUCCESS) {
    return nullptr;
  }
  return acl;
}

}  // namespace

// ------------------------------------------------------------------ processes

int RunQuiet(const std::vector<std::string>& args) {
  return RunProcess(args, /*capture=*/false, 0, nullptr);
}

std::string RunCapture(const std::vector<std::string>& args, std::size_t limit,
                       bool* exited_ok) {
  std::string out;
  const int code = RunProcess(args, /*capture=*/true, limit, &out);
  if (exited_ok != nullptr) {
    *exited_ok = code == 0;
  }
  return out;
}

// -------------------------------------------------------------------- desktop

std::string ReadClipboard(std::size_t limit) {
  // No helper process and no display server: the clipboard is in the operating
  // system here, so this asks it. That is the whole reason ReadClipboard is a
  // role in the header rather than a wrapper around "run one of three tools" —
  // the POSIX shape has no meaning on this side and porting it would have meant
  // shipping a dependency on software nobody installs on Windows.
  if (::OpenClipboard(nullptr) == 0) {
    return {};  // another process holds it; it will be free again in a moment
  }

  std::string out;
  const HANDLE handle = ::GetClipboardData(CF_UNICODETEXT);
  if (handle != nullptr) {
    const auto* text = static_cast<const wchar_t*>(::GlobalLock(handle));
    if (text != nullptr) {
      out = Narrow(std::wstring(text));
      ::GlobalUnlock(handle);
    }
  }
  ::CloseClipboard();

  if (out.size() > limit) {
    out.resize(limit);
  }
  return out;
}

const char* ClipboardEmptyHint() { return "clipboard is empty"; }

bool OpenUrl(const std::string& url) {
  // The caller has already established that this is an ordinary https URL with
  // no whitespace, no control characters and no quoting metacharacters, which
  // is what makes this call safe. ShellExecuteW hands the URL to the registered
  // protocol handler by substituting it into that handler's own command
  // template — so this is a command line being built somewhere else, out of
  // reach, and the only defence available is that the string cannot contain
  // anything that would end an argument. Building the command line here instead
  // and running it would put that defence back under this file's control but
  // would also mean reimplementing protocol-handler lookup, which is worse.
  const std::wstring wide = Widen(url);
  const HINSTANCE result =
      ::ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  // ShellExecute's return is a status code wearing an HINSTANCE, and anything
  // above 32 is success. This is not a pointer and must not be treated as one.
  return reinterpret_cast<INT_PTR>(result) > 32;
}

// ---------------------------------------------------------------- filesystem

bool RestrictToOwner(const std::string& path, std::string* error) {
  std::error_code ec;
  const bool is_dir = std::filesystem::is_directory(path, ec);

  PACL acl = OwnerOnlyDacl(is_dir ? (GENERIC_ALL) : (GENERIC_READ | GENERIC_WRITE | DELETE),
                           /*inheritable=*/is_dir);
  if (acl == nullptr) {
    if (error != nullptr) {
      *error = "could not build an owner-only ACL for " + path;
    }
    return false;
  }

  // PROTECTED_DACL_SECURITY_INFORMATION is the load-bearing flag and the one
  // difference from chmod that a port will get wrong.
  //
  // Setting a DACL without it leaves the *inherited* entries in place beside the
  // one written here — and what a file under %APPDATA% inherits is typically an
  // entry granting Users. So the naive version grants the owner explicitly,
  // reports success, and leaves the token exactly as readable as it was. This
  // flag is what severs inheritance and makes the ACL below the whole list.
  std::wstring wide = Widen(path);
  const DWORD status = ::SetNamedSecurityInfoW(
      wide.data(), SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr,
      acl, nullptr);
  ::LocalFree(acl);

  if (status != ERROR_SUCCESS) {
    if (error != nullptr) {
      *error = "could not restrict " + path + " to its owner: " + LastErrorText(status);
    }
    return false;
  }
  return true;
}

std::string ExecutablePath() {
  // No fixed maximum: MAX_PATH is 260 and a path can be far longer than that,
  // and GetModuleFileNameW's way of reporting a truncation is to fill the
  // buffer and set ERROR_INSUFFICIENT_BUFFER rather than to fail, so a single
  // fixed call would silently return a path that is a prefix of the real one.
  std::wstring buffer(MAX_PATH, L'\0');
  while (true) {
    const DWORD got = ::GetModuleFileNameW(nullptr, buffer.data(),
                                           static_cast<DWORD>(buffer.size()));
    if (got == 0) {
      return {};
    }
    if (got < buffer.size()) {
      buffer.resize(got);
      return Narrow(buffer);
    }
    if (buffer.size() > 32768) {
      return {};  // longer than any path Windows accepts; something is wrong
    }
    buffer.resize(buffer.size() * 2);
  }
}

std::string HomeDir() {
  const std::string profile = Env("USERPROFILE");
  if (!profile.empty()) {
    return profile;
  }
  // The pair HOMEDRIVE/HOMEPATH is what a domain-joined machine sets when the
  // profile lives on a share, and ssh looks there too.
  const std::string drive = Env("HOMEDRIVE");
  const std::string path = Env("HOMEPATH");
  if (!drive.empty() && !path.empty()) {
    return drive + path;
  }
  return Env("HOME");  // set by MSYS2 and by anyone who came from a shell
}

std::string ConfigDir() {
  // %APPDATA% is the roaming per-user directory, which is where a settings file
  // belongs — not %LOCALAPPDATA%, which is explicitly the data a user does not
  // get back when they sign in on another machine. A config that follows the
  // person is the behaviour ~/.config has.
  const std::string appdata = Env("APPDATA");
  if (!appdata.empty()) {
    return (std::filesystem::path(appdata) / "gittop").string();
  }
  const std::string home = HomeDir();
  if (home.empty()) {
    return {};
  }
  return (std::filesystem::path(home) / "AppData" / "Roaming" / "gittop").string();
}

// ----------------------------------------------------------------- terminal

bool StdoutIsTerminal() {
  // GetConsoleMode rather than _isatty. _isatty is true for any character
  // device, which on Windows includes NUL and a serial port — and the question
  // being asked is whether an animation has somewhere to animate.
  const HANDLE handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD mode = 0;
  return ::GetConsoleMode(handle, &mode) != 0;
}

int ConsoleColorBits() {
  if (!StdoutIsTerminal()) {
    return 0;  // no console, so no escape sequence has anywhere to land
  }

  // RtlGetVersion rather than GetVersionEx. GetVersionEx has been shimmed since
  // Windows 8.1: it reports 6.2 to any binary without a compatibility manifest
  // declaring support for later releases, which would put every build below the
  // 10586 floor and make this answer 4 bits on every machine. RtlGetVersion is
  // the kernel's own copy and is not lied to.
  DWORD build = 0;
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  if (const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll")) {
    const auto rtl_get_version =
        reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")));
    if (rtl_get_version != nullptr) {
      RTL_OSVERSIONINFOW info{};
      info.dwOSVersionInfoSize = sizeof(info);
      if (rtl_get_version(&info) == 0) {
        build = info.dwBuildNumber;
      }
    }
  }

  if (build >= 15063) {
    return 24;  // 1703 and later render 24-bit colour directly
  }
  if (build >= 10586) {
    // Virtual terminal sequences work, but RGB is approximated onto the legacy
    // palette. Saying 4 here rather than 24 is the one place this file guesses
    // low, and it is right to: these builds have been out of support since 2018,
    // and the alternative is a theme that quantizes to something nobody chose.
    return 4;
  }
  // Older than any console that understands an escape sequence. Nothing gittop
  // draws will work here, and emitting colour codes would only add literal
  // garbage to a screen that is already unreadable.
  return 0;
}

// ----------------------------------------------------------------- time

std::int64_t TimestampFromUtc(std::tm& tm) {
  return static_cast<std::int64_t>(::_mkgmtime(&tm));
}

std::tm UtcFromTimestamp(std::int64_t seconds) {
  const auto value = static_cast<std::time_t>(seconds);
  std::tm out{};
  // Arguments reversed from gmtime_r, and the return is an errno rather than a
  // pointer. Left zeroed on failure, which is what the POSIX side does too when
  // gmtime_r returns null.
  ::gmtime_s(&out, &value);
  return out;
}

// ------------------------------------------------------------- environment

bool SetEnv(const char* name, const char* value) {
  // Both, and this is the trap. A process has two environments here: the Win32
  // block, which is what a child created by CreateProcess inherits, and the C
  // runtime's own copy, which is what getenv reads. _putenv_s keeps them in step
  // for the CRT that owns it — but libgit2 is a separate translation unit set
  // that may hold its own CRT state, and the ssh it exec's reads the Win32 block.
  // Writing one and not the other produces a variable that is set from whichever
  // side you did not look at, which is indistinguishable from it never being set.
  const bool crt = ::_putenv_s(name, value) == 0;
  const bool win32 = ::SetEnvironmentVariableW(Widen(name).c_str(), Widen(value).c_str()) != 0;
  return crt && win32;
}

void UnsetEnv(const char* name) {
  // An empty value is how _putenv_s removes a variable; SetEnvironmentVariableW
  // wants a null pointer for the same thing, and an empty string there would
  // leave the name defined-but-empty instead of gone.
  ::_putenv_s(name, "");
  ::SetEnvironmentVariableW(Widen(name).c_str(), nullptr);
}

// ------------------------------------------------------------------- ssh

bool MaySshAgentBeRunning() {
  // Always maybe. Windows OpenSSH's agent is a service reached over a fixed
  // named pipe rather than an address in the environment, so there is nothing
  // to read here that would settle it and `ssh-add -l` is the only answer
  // available. Saying no instead would ask everyone using the agent properly
  // for a passphrase they have already stopped typing.
  return true;
}

// -------------------------------------------------------------- secret channel

struct SecretServer::Impl {
  std::string secret;
  std::string endpoint;
  HANDLE pipe = INVALID_HANDLE_VALUE;
  HANDLE connected = nullptr;  // signalled by the overlapped ConnectNamedPipe
  HANDLE stop = nullptr;       // signalled by the destructor
  std::thread thread;
  std::atomic<bool> served{false};

  void Serve() {
    while (true) {
      OVERLAPPED overlapped{};
      overlapped.hEvent = connected;
      ::ResetEvent(connected);

      bool ready = false;
      if (::ConnectNamedPipe(pipe, &overlapped) != 0) {
        ready = true;
      } else {
        const DWORD error = ::GetLastError();
        if (error == ERROR_PIPE_CONNECTED) {
          // A client connected in the window between CreateNamedPipe and this
          // call. The event is never signalled in that case, so waiting on it
          // would hang until the destructor — this is the whole reason the
          // return value cannot simply be ignored.
          ready = true;
        } else if (error != ERROR_IO_PENDING) {
          return;
        }
      }

      if (!ready) {
        const HANDLE waits[] = {connected, stop};
        const DWORD which = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (which != WAIT_OBJECT_0) {
          ::CancelIo(pipe);
          return;  // the transfer ended
        }
      }

      // The DACL is the whole access check, exactly as the 0700 directory is on
      // the other platform: a peer that opened this pipe is already running as
      // this user.
      DWORD written = 0;
      if (::WriteFile(pipe, secret.data(), static_cast<DWORD>(secret.size()), &written,
                      nullptr) != 0 &&
          written == secret.size()) {
        served.store(true);
      }
      // Flush before disconnecting, or the client's read races the teardown and
      // gets whatever happens to have arrived rather than the whole secret.
      ::FlushFileBuffers(pipe);
      ::DisconnectNamedPipe(pipe);

      if (::WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) {
        return;
      }
    }
  }
};

SecretServer::SecretServer(std::string secret) : impl_(std::make_unique<Impl>()) {
  impl_->secret = std::move(secret);

  // The name carries the process id and a counter so two gittops, or two
  // transfers in one, cannot collide on it. FILE_FLAG_FIRST_PIPE_INSTANCE below
  // turns a collision into a failure rather than into two servers sharing a
  // name, which is the outcome that would let the wrong one answer.
  static std::atomic<unsigned> counter{0};
  impl_->endpoint = "\\\\.\\pipe\\gittop-askpass-" +
                    std::to_string(::GetCurrentProcessId()) + "-" +
                    std::to_string(counter.fetch_add(1));

  PACL acl = OwnerOnlyDacl(GENERIC_READ | GENERIC_WRITE, /*inheritable=*/false);
  if (acl == nullptr) {
    return;
  }
  SECURITY_DESCRIPTOR descriptor{};
  if (::InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) == 0 ||
      ::SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) == 0) {
    ::LocalFree(acl);
    return;
  }
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.lpSecurityDescriptor = &descriptor;
  sa.bInheritHandle = FALSE;

  const std::wstring wide = Widen(impl_->endpoint);
  impl_->pipe = ::CreateNamedPipeW(
      wide.c_str(),
      PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_WAIT, 1, static_cast<DWORD>(impl_->secret.size() + 64), 0, 0,
      &sa);
  ::LocalFree(acl);

  if (impl_->pipe == INVALID_HANDLE_VALUE) {
    return;
  }

  impl_->connected = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  impl_->stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (impl_->connected == nullptr || impl_->stop == nullptr) {
    ::CloseHandle(impl_->pipe);
    impl_->pipe = INVALID_HANDLE_VALUE;
    return;
  }

  impl_->thread = std::thread([this] { impl_->Serve(); });
}

SecretServer::~SecretServer() {
  // Manual-reset and set once: this is the only thing that ends Serve's wait,
  // and the join below waits on that thread forever, so it has to be a state
  // the thread cannot miss rather than a notification it might be looking away
  // from. That is the same requirement the POSIX side meets with a pipe whose
  // write end is closed.
  if (impl_->stop != nullptr) {
    ::SetEvent(impl_->stop);
  }
  // A thread parked in WaitForMultipleObjects wakes on the stop event, but one
  // already inside WriteFile to a client that stopped reading would not — so the
  // pipe is cancelled as well.
  if (impl_->pipe != INVALID_HANDLE_VALUE) {
    ::CancelIoEx(impl_->pipe, nullptr);
  }
  if (impl_->thread.joinable()) {
    impl_->thread.join();
  }
  for (HANDLE handle : {impl_->connected, impl_->stop}) {
    if (handle != nullptr) {
      ::CloseHandle(handle);
    }
  }
  // Closing the last handle is what removes the name: a named pipe is not a
  // file and there is nothing to unlink, so the endpoint stops existing here
  // rather than needing the directory removal the POSIX side does.
  if (impl_->pipe != INVALID_HANDLE_VALUE) {
    ::CloseHandle(impl_->pipe);
  }
}

bool SecretServer::ok() const { return impl_->pipe != INVALID_HANDLE_VALUE; }

bool SecretServer::served() const { return impl_->served.load(); }

const std::string& SecretServer::endpoint() const { return impl_->endpoint; }

std::string ReadSecretFrom(const std::string& endpoint) {
  if (endpoint.empty()) {
    return {};
  }
  const std::wstring wide = Widen(endpoint);

  // The single instance may be busy with the previous client for a moment.
  // WaitNamedPipe is the documented way to queue rather than to fail, and a
  // short bound is right: the server is in the same process tree and either
  // answers promptly or is not going to.
  HANDLE pipe = ::CreateFileW(wide.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0,
                              nullptr);
  if (pipe == INVALID_HANDLE_VALUE && ::GetLastError() == ERROR_PIPE_BUSY) {
    if (::WaitNamedPipeW(wide.c_str(), 5000) != 0) {
      pipe = ::CreateFileW(wide.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    }
  }
  if (pipe == INVALID_HANDLE_VALUE) {
    return {};
  }

  std::string secret;
  char buffer[4096];
  DWORD got = 0;
  while (secret.size() < 64 * 1024 &&
         ::ReadFile(pipe, buffer, sizeof(buffer), &got, nullptr) != 0 && got > 0) {
    secret.append(buffer, got);
  }
  ::CloseHandle(pipe);
  return secret;
}

}  // namespace gittop::platform
