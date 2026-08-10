#pragma once

#include <ftxui/dom/elements.hpp>
#include <string>

#include "model/remote.hpp"

namespace gittop::ui {

// The Remote view. `frame` advances once per animation tick and drives the
// spinner; nothing else in here moves, because a dashboard that animates while
// it has nothing to say is just noise.
//
// Every state this panel can be in is drawn deliberately: waiting, ready,
// failed, and "there is no remote gittop can read", which is a legitimate way
// to run and should not look like an error.
// `transports` is git::TransportSummary(), passed in rather than called here so
// this file stays clear of libgit2. It decides whether push and pull can work at
// all, which makes it worth a line even when nothing is wrong.
ftxui::Element RemotePanel(const model::RemoteSnapshot& snapshot, int width, int height,
                           int frame, const std::string& transports);

}  // namespace gittop::ui
