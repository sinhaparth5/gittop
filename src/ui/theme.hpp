#pragma once

#include <array>
#include <cstdint>
#include <ftxui/screen/color.hpp>
#include <string>

namespace gittop::ui {

struct Rgb {
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
};

// What the terminal can actually show. Every color in the program funnels
// through ToColor, so quantizing here is the whole of the fallback: no panel
// has to know it is running somewhere with sixteen colors.
enum class ColorDepth {
  None,       // NO_COLOR, or TERM=dumb. Shape and glyphs carry everything.
  Ansi16,     // the base sixteen, whatever the user's terminal has mapped them to
  Ansi256,    // the 6x6x6 cube plus the gray ramp
  TrueColor,  // 24-bit, what the palettes are authored in
};

ftxui::Color ToColor(Rgb c);

// Linear blend, t clamped to [0, 1]. Used for bar gradients and for fading a
// toast toward its background instead of snapping it off the screen.
Rgb Mix(Rgb a, Rgb b, float t);

// "#rrggbb" or "rrggbb". Returns false and leaves `out` alone on anything else,
// which is what lets a typo in a config file be reported rather than rendered.
bool ParseHexColor(const std::string& text, Rgb* out);

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

// The sixteen colors a palette is written in. Every semantic token above is
// derived from these, which is what keeps a new theme sixteen lines rather than
// thirty tokens and four hand-tuned ramps — and what lets the config file
// override a theme by naming the same roles.
struct Palette {
  std::string name;   // "tokyo-night", as typed in the config
  std::string label;  // "Tokyo Night", as shown on screen
  bool light = false;

  Rgb bg;
  Rgb surface;
  Rgb surface_alt;
  Rgb surface_raised;
  Rgb border;
  Rgb text;
  Rgb text_dim;
  Rgb text_faint;
  Rgb accent;
  Rgb green;
  Rgb yellow;
  Rgb blue;
  Rgb red;
  Rgb purple;
  Rgb cyan;
};

// The active theme's name and its display label.
const std::string& ThemeName();
const std::string& ThemeLabel();

// False on an unknown name, leaving the current theme in place. Case and
// separators are forgiving: "Tokyo Night", "tokyo_night" and "tokyonight" all
// reach the same palette.
bool SetTheme(const std::string& name);

// Advances to the next palette and returns its label, for the toast.
const std::string& NextTheme();

// One field of the active palette, by the same role name the struct uses
// ("bg", "accent", "green", …). False on a name that is not a role, so a
// mistyped config key can be reported instead of silently doing nothing.
bool OverrideColor(const std::string& role, Rgb value);

// NO_COLOR beats everything, then TERM=dumb, then COLORTERM=truecolor/24bit,
// then a TERM naming 256 colors. Anything else is assumed to have the base
// sixteen, which every terminal worth supporting does.
ColorDepth DetectColorDepth();
ColorDepth ColorDepthNow();
void SetColorDepth(ColorDepth depth);
std::string ColorDepthName(ColorDepth depth);

// "auto", "truecolor"/"24bit", "256", "16", "none"/"mono". False on anything
// else.
bool ParseColorDepth(const std::string& text, ColorDepth* out);

}  // namespace gittop::ui
