#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gittop::model {

// One line of a file as HEAD has it, and the commit that last changed it.
// `first` marks the top of a run of lines from one commit, which is where the
// panel prints the commit and leaves the gutter blank below it.
struct BlameLine {
  std::string id;  // full hex, so `enter` can hand it to a commit diff
  std::string author;
  std::int64_t time = 0;
  std::string summary;
  int lineno = 0;
  std::string text;
  bool first = false;
};

struct BlameSnapshot {
  std::string path;
  std::string error;
  std::vector<BlameLine> lines;
};

}  // namespace gittop::model
