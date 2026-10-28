#include <doctest/doctest.h>

#include <string>
#include <utility>
#include <vector>

#include "git/graph.hpp"

using gittop::git::AssignLanes;
using gittop::git::LaneWidth;
using gittop::model::Commit;

namespace {

Commit Make(std::string id, std::vector<std::string> parents) {
  Commit commit;
  commit.id = std::move(id);
  commit.parents = std::move(parents);
  return commit;
}

}  // namespace

TEST_CASE("a straight line is one lane") {
  std::vector<Commit> log{Make("c", {"b"}), Make("b", {"a"}), Make("a", {})};
  AssignLanes(log);
  for (const Commit& commit : log) {
    CHECK(commit.lane == 0);
  }
  CHECK(LaneWidth(log) >= 1);
}

TEST_CASE("a merge opens a second lane and closes it at the fork") {
  // Display order, newest first: m merges side into main, both off base.
  std::vector<Commit> log{
      Make("m", {"main", "side"}),
      Make("main", {"base"}),
      Make("side", {"base"}),
      Make("base", {}),
  };
  AssignLanes(log);
  CHECK(log[0].lane == 0);
  CHECK(log[1].lane == 0);
  CHECK(log[2].lane == 1);
  CHECK(log[3].lane == 0);
  CHECK(LaneWidth(log) >= 2);
}
