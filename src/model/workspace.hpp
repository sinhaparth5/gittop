#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace gittop::model {

// One working tree of the repository. The main one has no `name`: git keeps
// linked worktrees under .git/worktrees/<name>, and that name is the handle a
// prune takes, while the main tree is simply the repository.
struct Worktree {
  std::string name;
  std::string path;    // no trailing separator
  std::string branch;  // short name, or the short id of a detached HEAD
  bool detached = false;
  bool main = false;
  bool current = false;  // the one gittop has open
  bool locked = false;
  // Its directory is gone, so all that is left is git's record of it. This is
  // what `git worktree prune` removes, and it cannot have a branch or changes.
  bool missing = false;
  int changes = 0;  // files with any change, untracked included
};

// One submodule, as HEAD records it and as the working tree has it. Ids are
// short and empty where there is nothing: `recorded` with no commit in HEAD
// yet, `checked_out` before the first update clones it.
struct Submodule {
  std::string name;
  std::string path;
  std::string url;
  std::string recorded;
  std::string checked_out;
  bool cloned = false;
  bool dirty = false;  // changes inside the submodule's own working tree
};

struct Workspace {
  std::vector<Worktree> worktrees;
  std::vector<Submodule> submodules;
  std::size_t rows() const { return worktrees.size() + submodules.size(); }
};

}  // namespace gittop::model
