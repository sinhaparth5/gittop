#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "model/blame.hpp"
#include "model/conflict.hpp"
#include "model/diff.hpp"
#include "model/history.hpp"
#include "model/operation.hpp"
#include "model/stash.hpp"
#include "model/status.hpp"
#include "model/workspace.hpp"

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

// One hunk of a file's diff, or one line of it, as the diff view addresses it.
// `header` is the hunk header the caller was looking at: the diff is re-read
// before anything is applied, and a header that no longer matches means the
// file changed since, so the line under the cursor is not the one in the patch.
struct HunkPick {
  model::DiffSource source = model::DiffSource::Worktree;
  std::string path;
  std::size_t hunk = 0;
  std::string header;
  int line = -1;  // the n-th +/-/context line of the hunk; -1 takes all of it
};

// One edit to the history under HEAD, as the history view asks for it. Squash
// and fixup fold the commit into its parent; the moves swap it with its parent
// (down) or its child (up), because the history list draws newest first.
enum class Rewrite {
  Squash,
  Fixup,
  Drop,
  Reword,
  MoveDown,
  MoveUp,
};

// What a rewrite would touch, for the confirm to say before it happens.
// `pushed` counts the rewritten commits already reachable from the upstream,
// which is the part other people may have built on.
struct RewriteScope {
  std::size_t commits = 0;
  std::size_t pushed = 0;
  std::string error;  // why it cannot be done; empty when it can
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

  // Pure, and defined in git/blame.cpp. HEAD's copy of `path`, each line with
  // the commit that last touched it. A full history walk on a long-lived file,
  // so App reads it on a worker with a Repository of its own.
  model::BlameSnapshot ReadBlame(const std::string& path) const;

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

  // In git/message.cpp. `EditorCommand` is git's choice of editor as an argv;
  // `PrepareCommitMessage` writes COMMIT_EDITMSG and returns its path, and
  // `CommitEditedMessage` commits what the editor left there — or refuses an
  // empty message, which is how an editor says "never mind". `Amend` replaces
  // HEAD's summary, keeps its body and author, and takes in whatever is staged.
  std::vector<std::string> EditorCommand() const;
  std::string PrepareCommitMessage() const;
  // MERGE_MSG: what a stopped merge, cherry-pick or revert means to commit as.
  std::string PendingMessage() const;
  OpResult CommitEditedMessage();
  std::string HeadSummary(bool* pushed) const;  // empty with no commits yet
  OpResult Amend(const std::string& summary);

  // Stages a hunk or a line of the working-tree diff, or unstages one of the
  // staged diff, by applying a patch built from it to the index. In
  // git/diff.cpp. Only a modified file: a new, deleted or renamed one stages
  // whole from the status view.
  OpResult ApplyHunk(const HunkPick& pick);

  // Switches to `branch`. A remote-only one first gets a local branch of the
  // same short name tracking it, which is `git checkout -b topic --track
  // origin/topic`. SAFE, so an uncommitted change in the way stops it.
  OpResult Checkout(const model::Branch& branch);

  // In git/pick.cpp. Each commits what it applied, keeping a picked commit's
  // author; a conflict stops it with the operation on disk for the pane to
  // finish, and `applied` says how many of `ids` got in before that.
  OpResult CherryPick(const std::vector<std::string>& ids, std::size_t* applied);
  OpResult Revert(const std::string& id);

  // In git/conflict.cpp. `Merge` merges `branch` into HEAD: a fast-forward
  // when HEAD is behind, a merge commit when nothing conflicts, and otherwise a
  // stop with MERGE_HEAD on disk for the resolver. Refuses on a dirty tree.
  //
  // `ReadConflict` is one conflicted file, parsed for the resolver. The two
  // resolves rewrite the working-tree file and stage it once no marker is
  // left. A hunk names the line it starts on as the resolver saw it: the file
  // is re-read first, and a hunk that moved means the file was edited since,
  // so the block under the cursor is not the one that would be replaced.
  OpResult Merge(const model::Branch& branch);
  model::ConflictFile ReadConflict(const std::string& path) const;
  OpResult ResolveHunk(const std::string& path, std::size_t hunk, std::size_t begin,
                       model::Side side);
  OpResult ResolveFile(const std::string& path, model::Side side);
  // Stages a conflicted file as it stands, after an editor resolved it.
  // Refuses while a conflict marker is still in it.
  OpResult MarkResolved(const std::string& path);

  // In git/reflog.cpp. `ResetTo` moves HEAD and its branch to `id`, refusing
  // on a dirty tree or an operation in progress, and refusing rather than
  // overwriting an untracked file the target has.
  std::vector<model::ReflogEntry> ReadReflog(std::size_t limit) const;
  OpResult ResetTo(const std::string& id);

  // In git/refs.cpp. `from` is anything rev-parse takes. Nothing here is
  // forced: a name that is taken is refused rather than overwritten.
  OpResult CreateBranch(const std::string& name, const std::string& from);
  OpResult RenameBranch(const std::string& name, const std::string& to);
  bool BranchMerged(const std::string& name) const;
  OpResult DeleteBranch(const std::string& name);
  // Lightweight with an empty message, annotated otherwise.
  OpResult CreateTag(const std::string& id, const std::string& name, const std::string& message);

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

  // An interactive rebase of one edit, in git/rebase.cpp. It runs through the
  // same on-disk rebase as the above, so a conflict lands in the operation pane
  // and `RebaseContinue` and `RebaseAbort` finish it. Only the first-parent
  // line of HEAD can be edited, and never across a merge: libgit2 drops merge
  // commits from a rebase without a word.
  RewriteScope PreviewRewrite(const std::string& id, Rewrite kind) const;
  OpResult RewriteHistory(const std::string& id, Rewrite kind, const std::string& summary);

  // In git/workspace.cpp. `ReadWorkspace` is every worktree, main one first,
  // and every submodule; a status read per worktree, so it is read on the way
  // into its view rather than at startup. `AddWorktree` checks `branch` out
  // beside the main tree, making the branch at HEAD if it does not exist.
  // `RemoveWorktree` deletes a linked worktree's directory and git's record of
  // it, refusing one that is locked or has changes; a missing one is pruned.
  model::Workspace ReadWorkspace() const;
  OpResult AddWorktree(const std::string& branch);
  OpResult RemoveWorktree(const std::string& name);

  std::string WorkdirPath() const;

  // Whether a change at `rel` (relative to the workdir, '/'-separated, a
  // trailing '/' for a directory) is noise to a file watcher: an ignored path,
  // or the parts of .git that churn without changing anything on screen —
  // objects, logs, and lock files, whose rename into place is the event that
  // counts.
  bool IsWatchNoise(const std::string& rel) const;

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

// How many of the newest reflog entries one undo takes back: one, except that
// a rebase writes an entry per commit and is still one action, so its whole
// run back to where it started counts. Zero for an empty log.
std::size_t UndoSteps(const std::vector<model::ReflogEntry>& log);

// The conflict blocks in a file's text, in git's marker format, two-way or
// diff3. Pure, and separate from ReadConflict so it can be tested on a string.
// A block with no closing marker is left out rather than guessed at.
// ponytail: assumes the default marker size of 7; a conflict-marker-size
// attribute would need reading from .gitattributes.
void ParseConflictMarkers(const std::string& text, model::ConflictFile* out);

// The text with hunk `hunk` replaced by `side`'s lines.
std::string ResolveConflictText(const model::ConflictFile& file, std::size_t hunk,
                                model::Side side);

}  // namespace gittop::git
