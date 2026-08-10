#pragma once

#include <ftxui/dom/elements.hpp>
#include <vector>

#include "model/history.hpp"

namespace gittop::ui {

// `rows` is filled during layout with the box each row landed in, so a click can
// be resolved back to an index. Null when nobody is asking.
ftxui::Element CommitList(const model::HistorySnapshot& history, int selected,
                          std::vector<ftxui::Box>* rows = nullptr);

// Trailing-window heatmap, one column per week and one row per weekday, laid
// out the way a contribution graph is so it reads without a legend lesson.
ftxui::Element ActivityPanel(const model::HistorySnapshot& history);

ftxui::Element BranchList(const model::HistorySnapshot& history, int selected,
                          std::vector<ftxui::Box>* rows = nullptr);

}  // namespace gittop::ui
