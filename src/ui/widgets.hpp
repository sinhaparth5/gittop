#pragma once

#include <ftxui/dom/elements.hpp>
#include <string>

#include "ui/theme.hpp"

namespace gittop::ui {

// A bar whose fill is shaded along a ramp and drawn with eighth-blocks, so a
// 40% value looks 40% full even in a twelve-cell box. FTXUI's stock gauge is a
// single flat color and quantises to whole cells, which is what made the old
// summary row look blocky.
ftxui::Element GradientBar(float ratio, Ramp ramp, Swatch trough);

// Key hint drawn as a raised chip. Keeping this in one place is what stops the
// footer and the overlays from drifting apart.
ftxui::Element Chip(const std::string& key, const std::string& label);

// Path with the directory dimmed and the filename bright, so a long list scans
// by filename instead of by prefix.
ftxui::Element PathText(const std::string& path, bool emphasised);

// A remote URL with its userinfo replaced, for anywhere one reaches the screen.
// A remote configured as https://user:token@host/... has the token in its URL,
// and there is exactly one rule about those: never render one. Shared rather
// than private to the remote panel because the push confirmation prints a URL
// too, and the second caller is where a rule like this quietly stops holding.
std::string SafeUrl(const std::string& url);

// One frame of the braille spinner. `frame` is App's counter, which advances on
// wall time rather than per rendered frame so the spin rate does not follow the
// frame rate. Negative values are handled, because the counter is a plain int
// and will eventually wrap.
std::string SpinnerFrame(int frame);

}  // namespace gittop::ui
