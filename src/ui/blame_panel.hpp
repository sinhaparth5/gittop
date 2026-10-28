#pragma once

#include <ftxui/dom/elements.hpp>

#include "model/blame.hpp"

namespace gittop::ui {

// `rows` is how many lines fit; only those around `selected` are built, the
// same window the diff panel draws, because a blame is as long as its file.
ftxui::Element BlamePanel(const model::BlameSnapshot& blame, bool loading, int selected,
                          int width, int rows);

}  // namespace gittop::ui
