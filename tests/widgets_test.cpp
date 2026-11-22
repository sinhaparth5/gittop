#include <doctest/doctest.h>

#include <cstdlib>
#include <string>

#include "platform/platform.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

using gittop::ui::Fit;
using gittop::ui::Rjust;
using gittop::ui::TextWidth;
using gittop::ui::Truncate;

TEST_CASE("width is measured in cells, not bytes") {
  CHECK(TextWidth("abc") == 3);
  CHECK(TextWidth("日本語") == 6);
  CHECK(TextWidth("") == 0);
}

TEST_CASE("truncation never tears a double-width glyph") {
  const std::string mixed = "a日本語b";
  for (int cells = 0; cells <= TextWidth(mixed) + 2; ++cells) {
    CAPTURE(cells);
    CHECK(TextWidth(Truncate(mixed, cells)) <= cells);
    // Fit and Rjust pad to the exact width whatever had to go, which is what
    // keeps the next column, and the right-hand border, where they were.
    CHECK(TextWidth(Fit(mixed, cells)) == cells);
    CHECK(TextWidth(Rjust(mixed, cells)) == cells);
  }
  CHECK(Truncate(mixed, TextWidth(mixed)) == mixed);
}

// Issue 13: what each terminal on the render-check list actually puts in the
// environment, as they ship by default. A wrong guess here is the failure most
// likely to go unnoticed — a 256-colour guess on a 24-bit terminal makes two
// themes quantize onto the same indices and `t` look like a broken key.
TEST_CASE("colour depth detection matches what each terminal advertises") {
  using gittop::ui::ColorDepth;
  using gittop::ui::DetectColorDepth;
  struct Case {
    const char* name;
    const char* term;
    const char* colorterm;  // nullptr: unset
    ColorDepth want;
  };
  const Case cases[] = {
      {"kitty", "xterm-kitty", "truecolor", ColorDepth::TrueColor},
      {"kitty, launcher strips COLORTERM", "xterm-kitty", nullptr, ColorDepth::TrueColor},
      {"alacritty", "alacritty", "truecolor", ColorDepth::TrueColor},
      {"alacritty, no COLORTERM", "alacritty", nullptr, ColorDepth::TrueColor},
      {"wezterm", "xterm-256color", "truecolor", ColorDepth::TrueColor},
      {"wezterm terminfo, no COLORTERM", "wezterm", nullptr, ColorDepth::TrueColor},
      {"GNOME Terminal / Ptyxis (VTE)", "xterm-256color", "truecolor", ColorDepth::TrueColor},
      // tmux only passes 24-bit through with an RGB/Tc override, so without
      // COLORTERM, 256 is the right answer rather than a pessimistic one.
      {"tmux inherits COLORTERM", "tmux-256color", "truecolor", ColorDepth::TrueColor},
      {"tmux over ssh, no COLORTERM", "tmux-256color", nullptr, ColorDepth::Ansi256},
      {"screen-256color tmux", "screen-256color", nullptr, ColorDepth::Ansi256},
      {"tmux-direct", "tmux-direct", nullptr, ColorDepth::TrueColor},
      {"linux console", "linux", nullptr, ColorDepth::Ansi16},
      {"dumb", "dumb", "truecolor", ColorDepth::None},
  };

  const char* vars[] = {"TERM", "COLORTERM", "NO_COLOR", "WT_SESSION"};
  std::string saved[4];
  bool had[4];
  for (int i = 0; i < 4; ++i) {
    const char* v = std::getenv(vars[i]);
    had[i] = v != nullptr;
    saved[i] = v ? v : "";
    gittop::platform::UnsetEnv(vars[i]);
  }

  for (const Case& c : cases) {
    CAPTURE(c.name);
    gittop::platform::SetEnv("TERM", c.term);
    if (c.colorterm != nullptr) {
      gittop::platform::SetEnv("COLORTERM", c.colorterm);
    } else {
      gittop::platform::UnsetEnv("COLORTERM");
    }
    CHECK(DetectColorDepth() == c.want);
  }

  for (int i = 0; i < 4; ++i) {
    if (had[i]) {
      gittop::platform::SetEnv(vars[i], saved[i].c_str());
    } else {
      gittop::platform::UnsetEnv(vars[i]);
    }
  }
}
