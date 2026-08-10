#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace gittop::git {

// What a transfer is doing right now. libgit2 reports counts rather than a
// phase, so these are inferred from which counter last moved — enough for a
// progress line to say something truer than "working".
enum class TransferPhase {
  Idle,
  Connecting,
  Receiving,   // objects coming down the wire
  Resolving,   // deltas being applied locally, no network involved
  Sending,     // objects going up
  Updating,    // refs and the working tree
  Done,
  Failed,
};

struct TransferProgress {
  TransferPhase phase = TransferPhase::Idle;

  int received_objects = 0;
  int indexed_objects = 0;
  int total_objects = 0;
  std::int64_t received_bytes = 0;

  int pushed_objects = 0;
  int total_push_objects = 0;

  // Whatever the server said out loud — the "Counting objects" chatter, and on
  // GitHub the pull request URL it prints after a push. Worth surfacing.
  std::string remote_message;

  // 0 to 1, or -1 when the totals are not known yet. A bar that jumps to a
  // number it made up is worse than a bar that admits it cannot say.
  float ratio() const {
    if (total_objects > 0 && phase == TransferPhase::Receiving) {
      return static_cast<float>(received_objects) / static_cast<float>(total_objects);
    }
    if (total_objects > 0 && phase == TransferPhase::Resolving) {
      return static_cast<float>(indexed_objects) / static_cast<float>(total_objects);
    }
    if (total_push_objects > 0 && phase == TransferPhase::Sending) {
      return static_cast<float>(pushed_objects) / static_cast<float>(total_push_objects);
    }
    return -1.0F;
  }
};

// Written from the worker and read from the UI thread every frame, which is the
// one piece of shared mutable state a transfer needs: the result comes back
// through the fetcher like every other, but progress has to be visible while
// the operation is still running.
//
// Held by shared_ptr rather than by App so the worker never reaches back into
// App from another thread, which is the rule the rest of the async layer keeps.
class ProgressSink {
 public:
  void Publish(const TransferProgress& progress);
  TransferProgress Read() const;

 private:
  mutable std::mutex mutex_;
  TransferProgress progress_;
};

// The one place a token leaves remote::Token: libgit2 wants a plain
// username/password pair for an https remote. Built per transfer and destroyed
// with it, and never logged, rendered, or stored anywhere longer-lived.
//
// Empty means anonymous, which is correct for a public fetch and for an ssh
// remote — with the exec transport the system ssh does its own authentication,
// against the user's own agent and keys.
struct Credentials {
  std::string username;
  std::string password;
};

// How a transfer ended. `summary` is the one line worth putting in the status
// bar; `detail` is what the progress overlay shows underneath it.
struct TransferResult {
  bool ok = false;
  std::string summary;
  std::string detail;
  std::string hint;

  // Set when the operation succeeded but changed nothing, which is a different
  // thing from failing and should not be reported in red.
  bool no_op = false;
};

// All three open their own git_repository from `repo_path`. libgit2 objects are
// not safe to share across threads, and App's Repository belongs to the UI
// thread, so a worker gets its own handle rather than borrowing one.

// Updates the remote-tracking refs and nothing else. The safe one: it can never
// touch the working tree or move a branch you are standing on.
TransferResult Fetch(const std::string& repo_path, const std::string& remote_name,
                     const Credentials& credentials, std::shared_ptr<ProgressSink> sink,
                     const std::atomic<bool>* cancel);

// Fetch, then fast-forward the current branch to its upstream if that is all it
// takes. A history that has genuinely diverged is left alone and reported:
// merging and rebasing are Phase 6, and doing either implicitly behind a
// one-key "pull" is how a dashboard loses somebody's work.
TransferResult Pull(const std::string& repo_path, const std::string& remote_name,
                    const Credentials& credentials, std::shared_ptr<ProgressSink> sink,
                    const std::atomic<bool>* cancel);

// Pushes the current branch, never with force. `set_upstream` mirrors
// `git push -u` for a branch that has no upstream yet.
TransferResult Push(const std::string& repo_path, const std::string& remote_name,
                    bool set_upstream, const Credentials& credentials,
                    std::shared_ptr<ProgressSink> sink, const std::atomic<bool>* cancel);

// True when the binary was built with a transport that can reach this URL.
// Checked before a transfer rather than after, so an ssh remote on a build
// without ssh support says so instead of failing deep inside libgit2.
bool TransportAvailable(const std::string& url, std::string* why_not);

// For the remote panel: "https, ssh" or whichever of them was compiled in.
std::string TransportSummary();

}  // namespace gittop::git
