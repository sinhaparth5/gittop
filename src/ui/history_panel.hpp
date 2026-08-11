#pragma once

#include <ftxui/dom/elements.hpp>
#include <vector>

#include "model/history.hpp"

namespace gittop::ui {

// `rows` is filled during layout with the box each row landed in, so a click can
// be resolved back to an index. Null when nobody is asking.
// `width` is the terminal's, used to work out how much room the summary has left
// after the lane gutter, the ref badges and the fixed right-hand columns. Every
// one of those is a known width, so the budget is arithmetic rather than a guess
// — which is what lets the summary end in an ellipsis instead of just stopping.
ftxui::Element CommitList(const model::HistorySnapshot& history, int selected, int width,
                          std::vector<ftxui::Box>* rows = nullptr);

// Trailing-window heatmap, one column per week and one row per weekday, laid
// out the way a contribution graph is so it reads without a legend lesson.
ftxui::Element ActivityPanel(const model::HistorySnapshot& history);

// `width` is the terminal's, not the panel's: the per-branch velocity sparkline
// is dropped below 96 columns rather than squeezing the name and the tracking
// counts, which are what the view is actually for.
ftxui::Element BranchList(const model::HistorySnapshot& history, int selected, int width,
                          std::vector<ftxui::Box>* rows = nullptr);

}  // namespace gittop::ui
