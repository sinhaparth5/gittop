#pragma once

#include <ftxui/dom/elements.hpp>

#include "model/remote.hpp"

namespace gittop::ui {

// The Remote view. `frame` advances once per animation tick and drives the
// spinner; nothing else in here moves, because a dashboard that animates while
// it has nothing to say is just noise.
//
// Every state this panel can be in is drawn deliberately: waiting, ready,
// failed, and "there is no remote gittop can read", which is a legitimate way
// to run and should not look like an error.
ftxui::Element RemotePanel(const model::RemoteSnapshot& snapshot, int width, int height,
                           int frame);

}  // namespace gittop::ui
