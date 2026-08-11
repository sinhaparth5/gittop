#pragma once

#include <ftxui/dom/elements.hpp>

#include "model/diff.hpp"

namespace gittop::ui {

// Where the cursor is and how much gutter to draw. Passed as a struct for the
// same reason the pipeline and pull views are: the panel needs several pieces
// of state that belong to App, and a fifth positional int is unreadable.
struct DiffView {
  int selected = 0;      // the line under the cursor, an index into DiffSnapshot::lines
  bool line_numbers = true;
  // Which side the diff came from is already in the snapshot; this is whether
  // the user can switch, which is false for a commit — a commit has no staged
  // and unstaged halves to toggle between.
  bool switchable = true;
};

ftxui::Element DiffPanel(const model::DiffSnapshot& diff, const DiffView& view, int width,
                         int height);

}  // namespace gittop::ui
