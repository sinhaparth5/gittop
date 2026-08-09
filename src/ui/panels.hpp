#pragma once

#include <ftxui/dom/elements.hpp>
#include <string>

#include "model/status.hpp"

namespace gittop::ui {

// Bar fill levels, kept apart from the snapshot because they ease toward the
// real counts across a few frames while the printed numbers stay exact. An
// animated digit is unreadable; an animated bar is the part worth smoothing.
struct StatBars {
  float staged = 0.0F;
  float unstaged = 0.0F;
  float untracked = 0.0F;
  float conflicted = 0.0F;
};

ftxui::Element Header(const model::StatusSnapshot& snapshot);
ftxui::Element SummaryRow(const model::StatusSnapshot& snapshot, const StatBars& bars);
ftxui::Element FileList(const model::StatusSnapshot& snapshot, int selected);

// `fade` runs 1 down to 0 as a message ages out.
ftxui::Element Footer(const std::string& message, bool is_error, float fade);

ftxui::Element HelpPane();
ftxui::Element ConfirmPane(const std::string& question, const std::string& detail);

ftxui::Decorator PaneFrame();

}  // namespace gittop::ui
