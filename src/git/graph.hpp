#pragma once

#include <vector>

#include "model/history.hpp"

namespace gittop::git {

// Walks the commit list in display order and assigns each one a lane, filling
// in the graph gutter for every row. Pure over the list: it reads only ids and
// parent ids, so it can be reasoned about and tested without a repository.
//
// The rule is the usual one. A lane is a promise to draw some commit later. On
// reaching a commit, every lane waiting on it converges: the leftmost carries
// the first parent onward and the others close. Additional parents take over an
// existing lane if one already waits on them, or open a new lane if not.
void AssignLanes(std::vector<model::Commit>& commits);

// Widest gutter across all rows, so the panel can align the log beside it.
int LaneWidth(const std::vector<model::Commit>& commits);

}  // namespace gittop::git
