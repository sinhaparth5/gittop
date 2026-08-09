#pragma once

#include <ftxui/dom/elements.hpp>
#include <string>

#include "model/status.hpp"

namespace gittop::ui {

ftxui::Element Header(const model::StatusSnapshot& snapshot);
ftxui::Element SummaryRow(const model::StatusSnapshot& snapshot);
ftxui::Element FileList(const model::StatusSnapshot& snapshot, int selected);
ftxui::Element Footer(const std::string& message, bool is_error);

ftxui::Element HelpPane();
ftxui::Element ConfirmPane(const std::string& question, const std::string& detail);

// Shared by the modal panes in app.cpp so overlays and panels stay visually
// consistent without app.cpp naming a color.
ftxui::Element KeyCap(const std::string& key);
ftxui::Decorator PaneFrame();

}  // namespace gittop::ui
