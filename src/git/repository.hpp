#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "model/diff.hpp"
#include "model/history.hpp"
#include "model/operation.hpp"
#include "model/stash.hpp"
#include "model/status.hpp"

struct git_repository;

namespace gittop::git {

// What ReadDiff should compare. A request rather than three methods, because
// the caller stores one of these to say which diff the view is showing and
// three methods would need a tag next to them saying the same thing.
struct DiffRequest {
  model::DiffSource source = model::DiffSource::Worktree;
  std::string path;    // limit to one file; empty diffs everything
  std::string commit;  // Source::Commit only; anything git rev-parse accepts
};

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

  // Also pure, and defined in git/diff.cpp. Capped at `max_lines` because a
  // vendored dependency landing in one commit is a diff no terminal is going to
  // scroll, and building the whole of it costs the same as showing it.
  model::DiffSnapshot ReadDiff(const DiffRequest& request, std::size_t max_lines) const;

  // Pure, and defined in git/stash.cpp. Cheap enough to read on every switch to
  // the view: the stash reflog is a handful of entries, not a revwalk.
  model::StashList ReadStashes() const;

  // Every branch and tag, by the short name a CI provider's ref filter matches.
  // Pure, and cheap: one `git_reference_list` and a peel per tag, with no
  // revwalk anywhere. Read on demand rather than at startup — nothing needs it
  // until somebody asks the CI view to look at a ref other than the one that is
  // checked out.
  std::vector<model::RefEntry> ReadRefs() const;

  // Pure, and defined in git/rebase.cpp. Read on every status refresh, because
  // a dashboard that does not notice an interrupted merge shows the conflicted
  // files and no reason for them.
  model::OperationState ReadOperation() const;

  OpResult Stage(const model::StatusEntry& entry);
  OpResult Unstage(const model::StatusEntry& entry);
  OpResult StageAll();
  // Only valid for worktree and untracked entries. Staged changes have to be
  // unstaged first, which keeps a single keystroke from destroying two things.
  OpResult Discard(const model::StatusEntry& entry);
  OpResult Commit(const std::string& message);

  // Stash mutations, in git/stash.cpp. Every one of them renumbers the entries
  // below it, so the caller re-reads the list rather than adjusting its own.
  OpResult StashSave(const std::string& message, bool include_untracked);
  OpResult StashApply(std::size_t index);
  OpResult StashPop(std::size_t index);
  OpResult StashDrop(std::size_t index);

  // Rebase, in git/rebase.cpp.
  //
  // `RebaseOntoUpstream` refuses on a dirty tree rather than stashing behind the
  // user's back: a rebase that quietly moves uncommitted work is how a tool
  // loses the one copy of something. `Continue` commits what is staged as the
  // stopped step and carries on, and stops again at the next conflict. `Abort`
  // puts the branch back where it started, which is the whole reason a rebase
  // is survivable at all.
  OpResult RebaseOntoUpstream();
  OpResult RebaseContinue();
  OpResult RebaseAbort();

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
