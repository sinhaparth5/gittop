#pragma once

#include <ftxui/dom/elements.hpp>

#include "model/history.hpp"

namespace gittop::ui {

ftxui::Element CommitList(const model::HistorySnapshot& history, int selected);

// Trailing-window heatmap, one column per week and one row per weekday, laid
// out the way a contribution graph is so it reads without a legend lesson.
ftxui::Element ActivityPanel(const model::HistorySnapshot& history);

ftxui::Element BranchList(const model::HistorySnapshot& history, int selected);

}  // namespace gittop::ui
