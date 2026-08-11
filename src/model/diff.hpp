#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "model/status.hpp"

namespace gittop::model {

// What a row of a diff is. Symbolic rather than "the line starts with +", so
// the panel decides how an addition looks and the reader only says what it
// found. It also means a hunk header is a kind rather than a string the panel
// has to recognise by its punctuation.
enum class DiffLineKind {
  Context,
  Added,
  Removed,
  FileHeader,  // the path banner gittop draws between files
  HunkHeader,  // @@ -a,b +c,d @@
  Binary,      // stands in for content that cannot be shown as text
  Note,        // anything gittop has to say about the diff itself
};

struct DiffLine {
  DiffLineKind kind = DiffLineKind::Context;
  std::string text;
  // -1 where the line does not exist on that side, which is what lets the
  // gutter print a blank for an addition's old number instead of a zero.
  int old_lineno = -1;
  int new_lineno = -1;
};

struct DiffFile {
  std::string path;
  std::string old_path;  // set only when change == Renamed
  Change change = Change::Modified;
  bool binary = false;
  std::size_t additions = 0;
  std::size_t deletions = 0;
  std::size_t first_line = 0;  // where its header sits in DiffSnapshot::lines
};

// Which comparison a diff came from. The panel prints it, and it is what makes
// an empty result readable: "nothing staged" and "no unstaged changes" are
// different answers to different questions, and a blank pane is neither.
enum class DiffSource {
  Worktree,  // index → working tree
  Staged,    // HEAD → index
  Commit,    // a commit against its first parent
};

// One flat list of lines rather than a tree of files holding hunks holding
// lines. Scrolling a diff is then a single integer; every alternative makes the
// cursor a pair that has to be kept agreeing with itself. `files` indexes back
// into `lines` so jumping to the next file stays one lookup.
struct DiffSnapshot {
  DiffSource source = DiffSource::Worktree;
  std::string title;     // "working tree", "staged", or a commit's subject
  std::string subtitle;  // the path it was limited to, or the commit id
  std::string error;

  std::vector<DiffFile> files;
  std::vector<DiffLine> lines;

  std::size_t additions = 0;
  std::size_t deletions = 0;

  // A diff large enough to hurt is cut off rather than rendered. The cap is
  // about what a terminal can scroll, not about what libgit2 can produce.
  bool truncated = false;

  bool empty() const { return files.empty(); }
};

}  // namespace gittop::model
