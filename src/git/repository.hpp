#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "model/history.hpp"
#include "model/status.hpp"

struct git_repository;

namespace gittop::git {

// Outcome of a mutating operation. Failures carry a message the UI shows in the
// status bar rather than throwing, because a rejected stage is an ordinary
// thing for a user to do, not an exceptional one.
struct OpResult {
  bool ok = true;
  std::string message;

  static OpResult Ok(std::string m = {}) { return {true, std::move(m)}; }
  static OpResult Fail(std::string m) { return {false, std::move(m)}; }
};

// Process-wide libgit2 init and shutdown. Construct exactly one, in main().
class Library {
 public:
  Library();
  ~Library();
  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;
};

class Repository {
 public:
  // Walks up from start_path looking for a working tree. Returns nullopt and
  // fills `error` when there is no repository, or when the one found is bare.
  static std::optional<Repository> Discover(const std::string& start_path, std::string* error);

  Repository(Repository&&) noexcept;
  Repository& operator=(Repository&&) noexcept;
  Repository(const Repository&) = delete;
  Repository& operator=(const Repository&) = delete;
  ~Repository();

  // Pure read. Touches no UI state and allocates its own result, so Phase 3 can
  // call it from a worker thread as long as one thread owns the Repository at a
  // time. libgit2 objects are not safe for concurrent use across threads.
  model::StatusSnapshot ReadStatus() const;

  // The other pure read. Walks every local branch tip, capped so a repository
  // with a hundred thousand commits still opens instantly; `truncated` says
  // whether the cap was reached. Same threading rule as ReadStatus.
  model::HistorySnapshot ReadHistory(std::size_t max_commits, int activity_days) const;

  OpResult Stage(const model::StatusEntry& entry);
  OpResult Unstage(const model::StatusEntry& entry);
  OpResult StageAll();
  // Only valid for worktree and untracked entries. Staged changes have to be
  // unstaged first, which keeps a single keystroke from destroying two things.
  OpResult Discard(const model::StatusEntry& entry);
  OpResult Commit(const std::string& message);

  std::string WorkdirPath() const;

  // Configured remotes as (name, fetch URL), in libgit2's order. Parsing a URL
  // into a provider is remote/provider.cpp's job, not this file's: git has no
  // opinion about what is on the other end and neither does this wrapper.
  std::vector<std::pair<std::string, std::string>> ReadRemotes() const;

 private:
  explicit Repository(git_repository* repo);

  struct Deleter {
    void operator()(git_repository* r) const;
  };
  std::unique_ptr<git_repository, Deleter> repo_;
};

}  // namespace gittop::git
