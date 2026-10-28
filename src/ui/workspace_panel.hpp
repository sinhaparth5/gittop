#pragma once

#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

#include "model/workspace.hpp"

namespace gittop::ui {

// Worktrees above submodules, with one cursor across both: `selected` counts
// the worktrees first. `rows` is filled with the box each row landed in, in the
// same order, so a click resolves to the same index the keys use. `add_key` is
// the live binding the empty state names.
ftxui::Element WorkspacePanel(const model::Workspace& workspace, int selected, int width,
                              const std::string& add_key,
                              std::vector<ftxui::Box>* rows = nullptr);

}  // namespace gittop::ui
