#pragma once

#include <ftxui/dom/elements.hpp>
#include <vector>

#include "model/pull.hpp"
#include "model/remote.hpp"

namespace gittop::ui {

// Cursor position and whether the detail pane is open. Unlike the CI view's
// drill-down, opening this one costs nothing: everything it shows came back
// with the list, so it is a disclosure rather than a request.
struct PullView {
  int selected = 0;
  bool details_open = false;
};

// `rows` is filled during layout with the box each row landed in, so a click
// can be turned back into an index. Null when nobody is asking.
ftxui::Element PullPanel(const model::PullSnapshot& snapshot, const model::RemoteRef& ref,
                         const PullView& view, int width, int height, int frame,
                         std::vector<ftxui::Box>* rows = nullptr);

}  // namespace gittop::ui
