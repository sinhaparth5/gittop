#pragma once

#include <ftxui/screen/color.hpp>

namespace gittop::ui {

// Semantic tokens. Panels name roles, never colors, so the Phase 7 visual pass
// swaps palettes by editing this one file instead of every panel. A raw
// ftxui::Color literal anywhere under src/ui/ outside theme.cpp is a bug.
struct Theme {
  ftxui::Color bg;
  ftxui::Color surface;
  ftxui::Color surface_alt;

  ftxui::Color border;
  ftxui::Color border_focus;

  ftxui::Color text;
  ftxui::Color text_dim;
  ftxui::Color text_faint;

  ftxui::Color accent;

  // Git-specific roles, kept separate from the generic status roles below so a
  // theme can tint "staged" without also changing every success message.
  ftxui::Color staged;
  ftxui::Color unstaged;
  ftxui::Color untracked;
  ftxui::Color conflict;

  ftxui::Color success;
  ftxui::Color warning;
  ftxui::Color danger;
};

const Theme& theme();

}  // namespace gittop::ui
