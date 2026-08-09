#include "git/graph.hpp"

#include <algorithm>
#include <cstddef>
#include <string>

namespace gittop::git {

using model::GraphCell;

void AssignLanes(std::vector<model::Commit>& commits) {
  // Each slot holds the id of the commit that lane is still waiting to draw.
  // An empty string means the lane is free for reuse.
  std::vector<std::string> lanes;

  const auto take_free_lane = [&lanes]() -> std::size_t {
    for (std::size_t i = 0; i < lanes.size(); ++i) {
      if (lanes[i].empty()) {
        return i;
      }
    }
    lanes.emplace_back();
    return lanes.size() - 1;
  };

  for (model::Commit& commit : commits) {
    // Everything waiting on this commit converges here.
    std::vector<std::size_t> waiting;
    for (std::size_t i = 0; i < lanes.size(); ++i) {
      if (lanes[i] == commit.id) {
        waiting.push_back(i);
      }
    }

    const std::size_t lane = waiting.empty() ? take_free_lane() : waiting.front();
    for (const std::size_t i : waiting) {
      lanes[i].clear();
    }

    // Snapshot the lanes still passing through before parents reoccupy any of
    // them, otherwise a lane that closes here would be drawn as continuing.
    std::vector<bool> through(lanes.size(), false);
    for (std::size_t i = 0; i < lanes.size(); ++i) {
      through[i] = !lanes[i].empty();
    }

    std::vector<std::size_t> opened;
    if (!commit.parents.empty()) {
      lanes[lane] = commit.parents.front();

      for (std::size_t p = 1; p < commit.parents.size(); ++p) {
        const std::string& parent = commit.parents[p];
        const bool already_waiting =
            std::find(lanes.begin(), lanes.end(), parent) != lanes.end();
        if (already_waiting) {
          continue;
        }
        const std::size_t opened_lane = take_free_lane();
        lanes[opened_lane] = parent;
        opened.push_back(opened_lane);
      }
    }

    commit.row.assign(lanes.size(), GraphCell::Empty);
    for (std::size_t i = 0; i < lanes.size() && i < through.size(); ++i) {
      if (through[i]) {
        commit.row[i] = GraphCell::Through;
      }
    }
    for (const std::size_t i : waiting) {
      if (i != lane) {
        commit.row[i] = GraphCell::Merge;
      }
    }
    for (const std::size_t i : opened) {
      commit.row[i] = GraphCell::Branch;
    }
    commit.row[lane] = GraphCell::Node;
    commit.lane = static_cast<int>(lane);
  }
}

int LaneWidth(const std::vector<model::Commit>& commits) {
  std::size_t width = 0;
  for (const model::Commit& commit : commits) {
    // Trailing empties would pad the gutter with blanks for no reason.
    std::size_t used = 0;
    for (std::size_t i = 0; i < commit.row.size(); ++i) {
      if (commit.row[i] != GraphCell::Empty) {
        used = i + 1;
      }
    }
    width = std::max(width, used);
  }
  return static_cast<int>(width);
}

}  // namespace gittop::git
