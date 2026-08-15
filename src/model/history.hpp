#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace gittop::model {

// One column of the graph gutter on one row. The UI maps these to box-drawing
// characters; keeping them symbolic means the lane algorithm can be tested
// without caring how a lane is eventually drawn.
enum class GraphCell : std::uint8_t {
  Empty,
  Node,     // the commit itself
  Through,  // a lane passing this row untouched
  Merge,    // a lane that ends here because it was waiting on this commit
  Branch,   // a lane opening here for a second or later parent
};

struct Commit {
  std::string id;
  std::string short_id;
  std::string summary;
  std::string author;
  std::int64_t time = 0;
  std::vector<std::string> parents;
  std::vector<std::string> refs;  // branch and tag names pointing at this commit
  bool is_head = false;

  // Filled in by AssignLanes.
  int lane = 0;
  std::vector<GraphCell> row;
};

// How many weeks of commit counts a branch carries. Twelve is a quarter, which
// is long enough that a branch's rhythm shows and short enough to draw in a
// dozen cells beside its name.
inline constexpr std::size_t kVelocityWeeks = 12;

struct Branch {
  std::string name;
  std::string upstream;
  bool is_head = false;

  // `has_upstream` means the tracking ref resolved, which is the only case
  // where `ahead` and `behind` are counts of anything. `upstream_gone` is the
  // branch that still asks for an upstream in its config but whose tracking ref
  // no longer exists — deleted on the remote and pruned since. Both cannot be
  // true, and when the second one is, `upstream` carries the configured name,
  // because which upstream went away is the useful half of the fact.
  bool has_upstream = false;
  bool upstream_gone = false;

  std::size_t ahead = 0;
  std::size_t behind = 0;
  std::int64_t time = 0;

  // Commits on this branch per week, oldest first, ending with the week in
  // progress. `velocity_max` is zero when nothing landed in the window at all —
  // which is not the same as twelve zeroes from a branch that was never walked,
  // and the panel draws the two differently.
  std::array<int, kVelocityWeeks> velocity{};
  int velocity_max = 0;
  bool velocity_known = false;
};

// A ref a CI provider might have attributed a run to. Branches and tags share
// one list and one field because the providers' filters take a single string
// and do not care which kind it names: a tag-triggered GitHub run carries the
// tag in `head_branch`, and a GitLab tag pipeline carries it in `ref`. Names
// are short — `master`, `v2026.08.3` — because that is what those filters
// match, not `refs/heads/master`.
struct RefEntry {
  std::string name;
  bool is_tag = false;
};

struct HistorySnapshot {
  std::vector<Commit> commits;
  std::vector<Branch> branches;

  // One bucket per day, oldest first, covering the trailing window.
  std::vector<int> activity;
  int activity_max = 0;
  std::int64_t activity_start_day = 0;  // epoch day of activity[0]

  // The full daily series, from the oldest commit walked through to today, with
  // gaps filled as zeroes. The chart view pans over this; the heatmap uses the
  // short window above. Contiguous rather than sparse so a chart can index it
  // by offset without searching.
  std::vector<int> daily;
  std::int64_t daily_start_day = 0;

  std::vector<std::pair<std::string, int>> authors;  // most commits first
  std::array<int, 7> weekday{};                      // index 0 is Sunday
  std::array<int, 24> hour{};                        // commit's own timezone

  std::size_t walked = 0;   // commits visited, which the log may have capped
  bool truncated = false;   // the walk hit its ceiling before running out

  bool empty() const { return commits.empty(); }
};

}  // namespace gittop::model
