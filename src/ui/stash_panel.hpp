#pragma once

#include <ftxui/dom/elements.hpp>
#include <vector>

#include "model/stash.hpp"

namespace gittop::ui {

// `rows` is filled during layout with the box each row landed in, the same
// contract every other clickable list here has.
ftxui::Element StashList(const model::StashList& stashes, int selected,
                         std::vector<ftxui::Box>* rows = nullptr);

}  // namespace gittop::ui
