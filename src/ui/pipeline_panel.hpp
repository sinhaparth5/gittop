#pragma once

#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

#include "model/history.hpp"
#include "model/pipeline.hpp"
#include "model/remote.hpp"

namespace gittop::ui {

// Everything the panel needs to know that is not in the data itself: where the
// cursor is, whether the drill-down is open, and what the refresh loop is
// doing. Passed as one struct so adding a field later is not another parameter
// on a function that already takes six.
struct PipelineView {
  int selected = 0;
  bool jobs_open = false;

  // Seconds until the next automatic refresh, or -1 when nothing is scheduled.
  // The panel says so out loud: a dashboard that silently stops updating is
  // worse than one that never updated.
  int next_refresh = -1;
  bool auto_paused = false;      // budget too low to keep polling
  std::string paused_reason;

  // An empty `PipelineSnapshot::branch` is two different facts — every ref was
  // asked for on purpose, or HEAD is detached and there was no branch to ask
  // for — and they produce the identical request. The header is the one place
  // that has to tell them apart, which is what this is for.
  bool all_refs_pinned = false;

  // Whatever key opens the ref picker, since it is rebindable and the empty
  // state is the one place a user most needs to be told it exists — an empty
  // run list is exactly what a repository with no CI at all looks like.
  std::string ref_key = "b";
};

// The ref the run list is filtered to, and the two rows that are not refs.
// Index 0 is "current branch" and index 1 is "all refs"; everything after them
// indexes the ref list, which is why the count is entries + 2 in exactly one
// place. Keeping the two specials inside the same selection rather than beside
// it means one integer describes the whole picker.
inline constexpr int kRefPickerSpecials = 2;

struct RefPickerView {
  int selected = 0;

  // What is filtered now, so the picker can mark it. `active` empty with
  // `active_all` false means the current branch row.
  std::string active;
  bool active_all = false;

  // The checked-out branch, empty when HEAD is detached or unborn. The
  // "current branch" row names it rather than saying "current branch" and
  // leaving you to guess which one that is.
  std::string head_branch;
};

ftxui::Element RefPickerPane(const std::vector<model::RefEntry>& entries,
                             const RefPickerView& view, int width, int height);

ftxui::Element PipelinePanel(const model::PipelineSnapshot& snapshot, const model::JobList& jobs,
                             const model::RemoteRef& ref, const PipelineView& view, int width,
                             int height, int frame, std::vector<ftxui::Box>* rows = nullptr);

}  // namespace gittop::ui
