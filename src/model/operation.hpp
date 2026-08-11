#pragma once

#include <cstddef>
#include <string>

namespace gittop::model {

// A repository can be in the middle of something. git leaves the state on disk
// and expects the next command to notice, which means a dashboard that does not
// look reports a merge conflict as an ordinary pile of unstaged files.
enum class Operation {
  None,
  Merge,
  Revert,
  CherryPick,
  Bisect,
  Rebase,
  ApplyMailbox,  // git am, and the am/rebase hybrid libgit2 reports as one state
};

// What is in progress and how far through it is. Step and total are zero when
// the operation does not count itself — only a rebase does, and only once its
// plan has been written.
struct OperationState {
  Operation operation = Operation::None;
  std::size_t step = 0;   // 1-based, so it reads as "3 of 7"
  std::size_t total = 0;
  std::string detail;     // the branch being rebased, or what it is going onto

  bool active() const { return operation != Operation::None; }
};

// Names the operation for a banner. Inline and here rather than in a .cpp
// because model/ is headers only: these types are the vocabulary the rest of
// the program shares and nothing in them should need linking against.
inline const char* OperationName(Operation op) {
  switch (op) {
    case Operation::Merge:
      return "merge";
    case Operation::Revert:
      return "revert";
    case Operation::CherryPick:
      return "cherry-pick";
    case Operation::Bisect:
      return "bisect";
    case Operation::Rebase:
      return "rebase";
    case Operation::ApplyMailbox:
      return "am";
    case Operation::None:
      break;
  }
  return "";
}

}  // namespace gittop::model
