#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace gittop::model {

// Which version of a conflicted region to keep. `Both` is ours then theirs,
// which is what most "keep both" resolutions of two added lines want and the
// rest are one edit away in the editor.
enum class Side {
  Ours,
  Theirs,
  Both,
};

// One `<<<<<<< ... >>>>>>>` block, by line index into ConflictFile::lines.
// `base` is only filled for a diff3-style block, which carries the common
// ancestor between `|||||||` and `=======`.
struct ConflictHunk {
  std::size_t begin = 0;  // the <<<<<<< line
  std::size_t end = 0;    // the >>>>>>> line, inclusive
  std::string ours_label;
  std::string theirs_label;
  std::vector<std::string> ours;
  std::vector<std::string> base;
  std::vector<std::string> theirs;
};

// A conflicted file as the resolver shows it. The working-tree file is the
// source of the hunks rather than the index's three stages, because it is the
// one the user may already have half-edited and the one being rewritten.
// Whether each side has the file at all comes from the index: a modify/delete
// conflict has no markers to parse and only a whole-file answer.
struct ConflictFile {
  std::string path;
  std::vector<std::string> lines;
  bool trailing_newline = true;
  std::vector<ConflictHunk> hunks;
  // A <<<<<<< with no >>>>>>> after it: left out of `hunks`, and the reason the
  // file is not clean even when `hunks` is empty.
  bool unclosed = false;
  bool ours_exists = true;
  bool theirs_exists = true;
  bool binary = false;
  std::string error;
};

}  // namespace gittop::model
