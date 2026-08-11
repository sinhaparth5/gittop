#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gittop::model {

// One entry of the stash reflog. `index` is its position in `stash@{n}` and is
// the only handle libgit2 offers, which makes it the thing every mutation takes
// — and the reason the list has to be re-read after any of them: dropping an
// entry renumbers every one below it.
struct Stash {
  std::size_t index = 0;
  std::string id;        // the stash commit's oid, for display only
  std::string short_id;
  std::string message;   // libgit2's "WIP on branch: abcdef summary"
  std::string branch;    // pulled back out of that message when it is there
  std::string summary;   // the rest of it, likewise
  std::int64_t time = 0;
};

struct StashList {
  std::vector<Stash> entries;
  bool empty() const { return entries.empty(); }
};

}  // namespace gittop::model
