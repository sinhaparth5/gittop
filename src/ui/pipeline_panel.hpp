#pragma once

#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

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
};

ftxui::Element PipelinePanel(const model::PipelineSnapshot& snapshot, const model::JobList& jobs,
                             const model::RemoteRef& ref, const PipelineView& view, int width,
                             int height, int frame, std::vector<ftxui::Box>* rows = nullptr);

}  // namespace gittop::ui
