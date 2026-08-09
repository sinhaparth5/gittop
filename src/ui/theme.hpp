#pragma once

#include <array>
#include <cstdint>
#include <ftxui/screen/color.hpp>

namespace gittop::ui {

struct Rgb {
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
};

ftxui::Color ToColor(Rgb c);

// Linear blend, t clamped to [0, 1]. Used for bar gradients and for fading a
// toast toward its background instead of snapping it off the screen.
Rgb Mix(Rgb a, Rgb b, float t);

// Converts implicitly, so panels keep writing color(theme().text) while the
// interpolating widgets reach the raw channels through .rgb.
struct Swatch {
  Rgb rgb;
  operator ftxui::Color() const { return ToColor(rgb); }  // NOLINT: intentional
};

// Two-stop ramp for bar fills. Hue travels with length the way btop shades a
// load bar, so a bar carries its reading before the number is parsed.
struct Ramp {
  Rgb from;
  Rgb to;
};

// Semantic tokens. Panels name roles, never colors, so a new palette is an edit
// to theme.cpp alone. A raw literal anywhere else under src/ui/ is a bug.
struct Theme {
  Swatch bg;
  Swatch surface;
  Swatch surface_alt;     // selected row
  Swatch surface_raised;  // key chips, input field

  Swatch border;
  Swatch border_focus;

  Swatch text;
  Swatch text_dim;
  Swatch text_faint;

  Swatch accent;

  Swatch staged;
  Swatch unstaged;
  Swatch untracked;
  Swatch conflict;

  Swatch success;
  Swatch warning;
  Swatch danger;

  Ramp staged_ramp;
  Ramp unstaged_ramp;
  Ramp untracked_ramp;
  Ramp conflict_ramp;

  // Cycled by lane index in the commit graph. Six distinct hues is enough that
  // adjacent lanes never collide in practice, and few enough that they all stay
  // legible on the same background.
  std::array<Swatch, 6> graph;
};

const Theme& theme();

}  // namespace gittop::ui
