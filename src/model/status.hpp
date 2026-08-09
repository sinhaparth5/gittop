#pragma once

#include <cstddef>
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

// A read-only picture of the repository at one moment. Nothing in here refers
// to libgit2 or to FTXUI, which is what lets Phase 3 build it on a worker
// thread and hand it to the UI without either side knowing about the other.
struct StatusSnapshot {
  std::string repo_name;
  std::string branch;
  bool head_unborn = false;
  bool head_detached = false;

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
