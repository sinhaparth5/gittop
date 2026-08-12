#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gittop::model {

// Where a change lives. One file can be listed twice, once for a staged change
// and once for an unstaged one, because staging acts on exactly one of them.
enum class Stage {
  Index,
  Worktree,
  Conflict,
};

enum class Change {
  None,
  Added,
  Modified,
  Deleted,
  Renamed,
  TypeChange,
  Untracked,
};

struct StatusEntry {
  std::string path;
  std::string old_path;  // set only when change == Renamed
  Stage stage = Stage::Worktree;
  Change change = Change::None;
};

// How many commits the status read carries for its sidecar. Twelve fills the
// sidecar on a tall terminal and is thrown away on a short one — and, unlike
// the history view's walk, it is a *bound*, so the read stays O(12) rather than
// O(repository). That is the whole reason the Status view may have a commit
// list at all without breaking the rule that opening a repository pays for no
// revwalk nobody asked to see.
inline constexpr std::size_t kRecentCommits = 12;

struct RecentCommit {
  std::string short_id;
  std::string summary;
  std::string author;
  std::int64_t time = 0;
};

// A read-only picture of the repository at one moment. Nothing in here refers
// to libgit2 or to FTXUI, which is what lets Phase 3 build it on a worker
// thread and hand it to the UI without either side knowing about the other.
struct StatusSnapshot {
  std::string repo_name;
  std::string branch;
  bool head_unborn = false;
  bool head_detached = false;

  // The tracking picture, read straight from the branch rather than from a
  // fetch, so it says what the last fetch knew and not what the server has.
  std::string upstream;
  bool has_upstream = false;
  std::size_t ahead = 0;
  std::size_t behind = 0;

  // Newest first. Empty on an unborn head, which is not the same as a
  // repository whose walk failed — both draw as the empty state.
  std::vector<RecentCommit> recent;

  // Ordered conflicts first, then staged, then unstaged, then untracked.
  std::vector<StatusEntry> entries;

  std::size_t staged = 0;
  std::size_t unstaged = 0;
  std::size_t untracked = 0;
  std::size_t conflicted = 0;

  std::size_t total() const { return staged + unstaged + untracked + conflicted; }
  bool clean() const { return entries.empty(); }
};

}  // namespace gittop::model
