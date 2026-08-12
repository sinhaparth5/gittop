#include "ui/theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <ftxui/screen/terminal.hpp>

namespace gittop::ui {
namespace {

constexpr Rgb C(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
  return Rgb{r, g, b};
}

// Palettes are authored in the sixteen roles of ui::Palette and composed into
// the semantic tokens below. Writing them this way is not just brevity: it is
// what makes a user theme in the config file the same object as a built-in one,
// since both are a set of role names with colors against them.
//
// The first entry is the default, and the order here is the order `t` cycles.
const Palette kPalettes[] = {
    // Deep, slightly blue ground with three surface steps above it. Three steps
    // is enough to separate panel from selection from chip without any of them
    // reading as a different color.
    {
        .name = "default",
        .label = "gittop",
        .light = false,
        .bg = C(0x0b, 0x0f, 0x16),
        .surface = C(0x13, 0x19, 0x24),
        .surface_alt = C(0x1e, 0x26, 0x36),
        .surface_raised = C(0x27, 0x31, 0x45),
        .border = C(0x26, 0x30, 0x43),
        .text = C(0xd7, 0xde, 0xe8),
        .text_dim = C(0x97, 0xa3, 0xb6),
        .text_faint = C(0x5e, 0x6b, 0x7f),
        .accent = C(0xf0, 0x88, 0x3e),
        .green = C(0x56, 0xd3, 0x64),
        .yellow = C(0xe3, 0xb3, 0x41),
        .blue = C(0x79, 0xc0, 0xff),
        .red = C(0xff, 0x7b, 0x72),
        .purple = C(0xd2, 0xa8, 0xff),
        .cyan = C(0x39, 0xc5, 0xcf),
    },
    {
        .name = "catppuccin",
        .label = "Catppuccin Mocha",
        .light = false,
        .bg = C(0x18, 0x18, 0x25),
        .surface = C(0x1e, 0x1e, 0x2e),
        .surface_alt = C(0x31, 0x32, 0x44),
        .surface_raised = C(0x45, 0x47, 0x5a),
        .border = C(0x31, 0x32, 0x44),
        .text = C(0xcd, 0xd6, 0xf4),
        .text_dim = C(0xa6, 0xad, 0xc8),
        .text_faint = C(0x6c, 0x70, 0x86),
        .accent = C(0xfa, 0xb3, 0x87),
        .green = C(0xa6, 0xe3, 0xa1),
        .yellow = C(0xf9, 0xe2, 0xaf),
        .blue = C(0x89, 0xb4, 0xfa),
        .red = C(0xf3, 0x8b, 0xa8),
        .purple = C(0xcb, 0xa6, 0xf7),
        .cyan = C(0x94, 0xe2, 0xd5),
    },
    {
        .name = "gruvbox",
        .label = "Gruvbox Dark",
        .light = false,
        .bg = C(0x1d, 0x20, 0x21),
        .surface = C(0x28, 0x28, 0x28),
        .surface_alt = C(0x3c, 0x38, 0x36),
        .surface_raised = C(0x50, 0x49, 0x45),
        .border = C(0x3c, 0x38, 0x36),
        .text = C(0xeb, 0xdb, 0xb2),
        .text_dim = C(0xbd, 0xae, 0x93),
        .text_faint = C(0x7c, 0x6f, 0x64),
        .accent = C(0xfe, 0x80, 0x19),
        .green = C(0xb8, 0xbb, 0x26),
        .yellow = C(0xfa, 0xbd, 0x2f),
        .blue = C(0x83, 0xa5, 0x98),
        .red = C(0xfb, 0x49, 0x34),
        .purple = C(0xd3, 0x86, 0x9b),
        .cyan = C(0x8e, 0xc0, 0x7c),
    },
    {
        .name = "nord",
        .label = "Nord",
        .light = false,
        .bg = C(0x2e, 0x34, 0x40),
        .surface = C(0x3b, 0x42, 0x52),
        .surface_alt = C(0x43, 0x4c, 0x5e),
        .surface_raised = C(0x4c, 0x56, 0x6a),
        .border = C(0x43, 0x4c, 0x5e),
        .text = C(0xec, 0xef, 0xf4),
        .text_dim = C(0xd8, 0xde, 0xe9),
        .text_faint = C(0x7b, 0x88, 0xa1),
        .accent = C(0x88, 0xc0, 0xd0),
        .green = C(0xa3, 0xbe, 0x8c),
        .yellow = C(0xeb, 0xcb, 0x8b),
        .blue = C(0x81, 0xa1, 0xc1),
        .red = C(0xbf, 0x61, 0x6a),
        .purple = C(0xb4, 0x8e, 0xad),
        .cyan = C(0x8f, 0xbc, 0xbb),
    },
    {
        .name = "tokyo-night",
        .label = "Tokyo Night",
        .light = false,
        .bg = C(0x1a, 0x1b, 0x26),
        .surface = C(0x1f, 0x23, 0x35),
        .surface_alt = C(0x29, 0x2e, 0x42),
        .surface_raised = C(0x3b, 0x42, 0x61),
        .border = C(0x2f, 0x35, 0x49),
        .text = C(0xc0, 0xca, 0xf5),
        .text_dim = C(0xa9, 0xb1, 0xd6),
        .text_faint = C(0x56, 0x5f, 0x89),
        .accent = C(0xff, 0x9e, 0x64),
        .green = C(0x9e, 0xce, 0x6a),
        .yellow = C(0xe0, 0xaf, 0x68),
        .blue = C(0x7a, 0xa2, 0xf7),
        .red = C(0xf7, 0x76, 0x8e),
        .purple = C(0xbb, 0x9a, 0xf7),
        .cyan = C(0x7d, 0xcf, 0xff),
    },
    // Dracula has no blue of its own. Its published ANSI mapping puts the
    // purple in the blue slot and the pink in the magenta slot, which is what
    // every Dracula terminal profile does, so that is what is copied here
    // rather than inventing a sixteenth color the palette never had.
    {
        .name = "dracula",
        .label = "Dracula",
        .light = false,
        .bg = C(0x21, 0x22, 0x2c),
        .surface = C(0x28, 0x2a, 0x36),
        .surface_alt = C(0x34, 0x37, 0x46),
        .surface_raised = C(0x44, 0x47, 0x5a),
        .border = C(0x34, 0x37, 0x46),
        .text = C(0xf8, 0xf8, 0xf2),
        .text_dim = C(0xc4, 0xc6, 0xd4),
        .text_faint = C(0x62, 0x72, 0xa4),
        .accent = C(0xff, 0xb8, 0x6c),
        .green = C(0x50, 0xfa, 0x7b),
        .yellow = C(0xf1, 0xfa, 0x8c),
        .blue = C(0xbd, 0x93, 0xf9),
        .red = C(0xff, 0x55, 0x55),
        .purple = C(0xff, 0x79, 0xc6),
        .cyan = C(0x8b, 0xe9, 0xfd),
    },
    // The light theme is not the dark one inverted, and the two places that
    // shows are the surface scale and the hues. Panels here are *brighter* than
    // the page rather than darker, because paper lifts toward white; and every
    // accent is a deeper, more saturated color than its dark-theme counterpart,
    // because a pastel that reads clearly on near-black disappears on near-white.
    {
        .name = "daylight",
        .label = "Daylight",
        .light = true,
        .bg = C(0xf2, 0xef, 0xe7),
        .surface = C(0xfd, 0xfc, 0xf8),
        .surface_alt = C(0xe6, 0xe1, 0xd5),
        .surface_raised = C(0xd9, 0xd3, 0xc4),
        .border = C(0xcf, 0xc8, 0xb7),
        .text = C(0x24, 0x22, 0x1e),
        .text_dim = C(0x55, 0x50, 0x47),
        .text_faint = C(0x87, 0x80, 0x72),
        .accent = C(0xa8, 0x4b, 0x14),
        .green = C(0x2b, 0x6e, 0x2f),
        .yellow = C(0x7d, 0x59, 0x00),
        .blue = C(0x17, 0x55, 0xac),
        .red = C(0xa5, 0x22, 0x1b),
        .purple = C(0x66, 0x31, 0x9b),
        .cyan = C(0x00, 0x5f, 0x64),
    },
    {
        // Built against a dichromat simulation rather than by eye.
        //
        // Every other palette here puts staged on green and conflicted on red,
        // and under deuteranopia those two are the same colour: measured at
        // CIELAB ΔE 0.8 in the default theme and 0.7 for daylight's unstaged
        // against its conflict. gittop is readable anyway, because a status is
        // always drawn as a glyph and a letter as well as a colour — but "the
        // colour is redundant" is a weaker promise than "the colour works", and
        // this palette makes the second one true.
        //
        // The four states move off the red/green axis entirely and onto
        // blue/amber, separated by lightness as well as hue: worst-case ΔE 36.6
        // across normal vision, deuteranopia, protanopia and tritanopia.
        //
        // The six graph lanes are *not* all mutually distinct here, and cannot
        // be: a dichromat sees a roughly two-dimensional colour space and six
        // separated hues do not fit in it. That is acceptable where it is not
        // for the statuses, because a lane's colour is redundant with its
        // column — lane three is the third column whatever colour it is drawn.
        .name = "accessible",
        .label = "Accessible",
        .light = false,
        .bg = C(0x0b, 0x0f, 0x16),
        .surface = C(0x13, 0x19, 0x24),
        .surface_alt = C(0x1e, 0x26, 0x36),
        .surface_raised = C(0x27, 0x31, 0x45),
        .border = C(0x26, 0x30, 0x43),
        .text = C(0xd7, 0xde, 0xe8),
        .text_dim = C(0x97, 0xa3, 0xb6),
        .text_faint = C(0x5e, 0x6b, 0x7f),
        .accent = C(0x4e, 0xc9, 0xb0),
        .green = C(0x3d, 0x9b, 0xf0),   // staged, success, added lines
        .yellow = C(0xe8, 0xb3, 0x39),  // unstaged, warning
        .blue = C(0xc8, 0xe0, 0xf5),    // untracked
        .red = C(0xd9, 0x4f, 0x70),     // conflicted, danger, removed lines
        .purple = C(0xb0, 0x8c, 0xff),
        .cyan = C(0x7a, 0xd7, 0xc8),
    },
};

constexpr std::size_t kPaletteCount = sizeof(kPalettes) / sizeof(kPalettes[0]);

// "Tokyo Night", "tokyo_night" and "tokyonight" all have to reach the same
// palette: the name is something a person types into a config file, not an
// identifier.
std::string Normalize(const std::string& name) {
  std::string out;
  for (const char ch : name) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
      out.push_back(ch);
    } else if (ch >= 'A' && ch <= 'Z') {
      out.push_back(static_cast<char>(ch - 'A' + 'a'));
    }
  }
  return out;
}

Theme Compose(const Palette& p) {
  // A bar's fill starts near the page and travels toward the strongest form of
  // its own hue. On a light theme "strongest" is toward black, not toward
  // white, which is the one place the ramp cannot be palette-agnostic.
  const Rgb peak = p.light ? Rgb{0, 0, 0} : Rgb{255, 255, 255};
  const auto ramp = [&p, peak](Rgb base) {
    return Ramp{Mix(base, p.bg, 0.45F), Mix(base, peak, 0.28F)};
  };

  Theme t;
  t.bg = {p.bg};
  t.surface = {p.surface};
  t.surface_alt = {p.surface_alt};
  t.surface_raised = {p.surface_raised};

  t.border = {p.border};
  t.border_focus = {p.accent};

  t.text = {p.text};
  t.text_dim = {p.text_dim};
  t.text_faint = {p.text_faint};

  t.accent = {p.accent};

  t.staged = {p.green};
  t.unstaged = {p.yellow};
  t.untracked = {p.blue};
  t.conflict = {p.red};

  t.success = {p.green};
  t.warning = {p.yellow};
  t.danger = {p.red};

  // A diff is mostly context, so the changed rows carry a wash of their own
  // hue rather than the full colour: a screen where every other line is a solid
  // green block is harder to read than one where the eye can find the changes.
  // Mixing toward the surface rather than a fixed dark keeps the light theme's
  // tint light instead of muddy.
  t.diff_add = {p.green};
  t.diff_del = {p.red};
  t.diff_hunk = {p.cyan};
  t.diff_add_bg = {Mix(p.green, p.surface, 0.86F)};
  t.diff_del_bg = {Mix(p.red, p.surface, 0.86F)};

  t.staged_ramp = ramp(p.green);
  t.unstaged_ramp = ramp(p.yellow);
  t.untracked_ramp = ramp(p.blue);
  t.conflict_ramp = ramp(p.red);

  t.graph = {Swatch{p.blue},   Swatch{p.green},  Swatch{p.yellow},
             Swatch{p.purple}, Swatch{p.red},    Swatch{p.cyan}};
  return t;
}

// Active state. Written at startup and by the `t` key, both on the UI thread;
// read by every panel on that same thread during Render.
std::size_t g_index = 0;
Palette g_palette = kPalettes[0];
Theme g_theme = Compose(kPalettes[0]);
ColorDepth g_depth = ColorDepth::TrueColor;

void Recompose() {
  g_theme = Compose(g_palette);
}

// Perceptual enough for picking a neighbour out of a fixed palette, and far
// cheaper than a conversion to Lab. Weighting green heaviest and shifting the
// red/blue weights by where the pair sits on the red axis is what stops dark
// blues from collapsing onto black.
long Distance(Rgb a, Rgb b) {
  const long rmean = (static_cast<long>(a.r) + static_cast<long>(b.r)) / 2;
  const long dr = static_cast<long>(a.r) - static_cast<long>(b.r);
  const long dg = static_cast<long>(a.g) - static_cast<long>(b.g);
  const long db = static_cast<long>(a.b) - static_cast<long>(b.b);
  return (((512 + rmean) * dr * dr) >> 8) + 4 * dg * dg + (((767 - rmean) * db * db) >> 8);
}

int Nearest256(Rgb c) {
  // 16..231 is a 6x6x6 cube on these levels, 232..255 a 24-step gray ramp. The
  // gray ramp is finer than the cube's diagonal, so a near-neutral color has to
  // be tried against both rather than quantized straight into the cube.
  static constexpr int kLevels[6] = {0, 95, 135, 175, 215, 255};
  const auto axis = [](std::uint8_t v) {
    int best = 0;
    for (int i = 1; i < 6; ++i) {
      if (std::abs(kLevels[i] - int{v}) < std::abs(kLevels[best] - int{v})) {
        best = i;
      }
    }
    return best;
  };
  const int ri = axis(c.r);
  const int gi = axis(c.g);
  const int bi = axis(c.b);
  const Rgb cube{static_cast<std::uint8_t>(kLevels[ri]), static_cast<std::uint8_t>(kLevels[gi]),
                 static_cast<std::uint8_t>(kLevels[bi])};

  int index = 16 + (36 * ri) + (6 * gi) + bi;
  long best = Distance(c, cube);

  for (int i = 0; i < 24; ++i) {
    const auto level = static_cast<std::uint8_t>(8 + (10 * i));
    const long d = Distance(c, Rgb{level, level, level});
    if (d < best) {
      best = d;
      index = 232 + i;
    }
  }
  return index;
}

int Nearest16(Rgb c) {
  // xterm's defaults. A user who has remapped their sixteen gets their own
  // colors, which is the point of dropping to this palette rather than
  // approximating one.
  static const Rgb kBase[16] = {
      C(0x00, 0x00, 0x00), C(0xcd, 0x00, 0x00), C(0x00, 0xcd, 0x00), C(0xcd, 0xcd, 0x00),
      C(0x00, 0x00, 0xee), C(0xcd, 0x00, 0xcd), C(0x00, 0xcd, 0xcd), C(0xe5, 0xe5, 0xe5),
      C(0x7f, 0x7f, 0x7f), C(0xff, 0x00, 0x00), C(0x00, 0xff, 0x00), C(0xff, 0xff, 0x00),
      C(0x5c, 0x5c, 0xff), C(0xff, 0x00, 0xff), C(0x00, 0xff, 0xff), C(0xff, 0xff, 0xff),
  };
  int best = 0;
  long best_d = Distance(c, kBase[0]);
  for (int i = 1; i < 16; ++i) {
    const long d = Distance(c, kBase[i]);
    if (d < best_d) {
      best_d = d;
      best = i;
    }
  }
  return best;
}

bool Contains(const char* haystack, const char* needle) {
  return haystack != nullptr && std::strstr(haystack, needle) != nullptr;
}

}  // namespace

ftxui::Color ToColor(Rgb c) {
  switch (g_depth) {
    case ColorDepth::None:
      return {ftxui::Color::Default};
    case ColorDepth::Ansi16:
      return {static_cast<ftxui::Color::Palette16>(Nearest16(c))};
    case ColorDepth::Ansi256:
      return {static_cast<ftxui::Color::Palette256>(Nearest256(c))};
    case ColorDepth::TrueColor:
      break;
  }
  return ftxui::Color::RGB(c.r, c.g, c.b);
}

Rgb Mix(Rgb a, Rgb b, float t) {
  t = std::clamp(t, 0.0F, 1.0F);
  const auto lerp = [t](std::uint8_t x, std::uint8_t y) {
    const float v = static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t;
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0F, 255.0F)));
  };
  return Rgb{lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b)};
}

bool ParseHexColor(const std::string& text, Rgb* out) {
  std::string hex = text;
  if (!hex.empty() && hex.front() == '#') {
    hex.erase(hex.begin());
  }
  if (hex.size() != 6) {
    return false;
  }
  auto nibble = [](char ch, int* value) {
    if (ch >= '0' && ch <= '9') {
      *value = ch - '0';
    } else if (ch >= 'a' && ch <= 'f') {
      *value = ch - 'a' + 10;
    } else if (ch >= 'A' && ch <= 'F') {
      *value = ch - 'A' + 10;
    } else {
      return false;
    }
    return true;
  };

  int channels[3] = {0, 0, 0};
  for (int i = 0; i < 3; ++i) {
    int hi = 0;
    int lo = 0;
    if (!nibble(hex[static_cast<std::size_t>(i * 2)], &hi) ||
        !nibble(hex[static_cast<std::size_t>((i * 2) + 1)], &lo)) {
      return false;
    }
    channels[i] = (hi * 16) + lo;
  }
  *out = Rgb{static_cast<std::uint8_t>(channels[0]), static_cast<std::uint8_t>(channels[1]),
             static_cast<std::uint8_t>(channels[2])};
  return true;
}

const Theme& theme() {
  return g_theme;
}

const std::string& ThemeName() {
  return g_palette.name;
}

const std::string& ThemeLabel() {
  return g_palette.label;
}

bool SetTheme(const std::string& name) {
  const std::string wanted = Normalize(name);
  for (std::size_t i = 0; i < kPaletteCount; ++i) {
    if (Normalize(kPalettes[i].name) == wanted) {
      g_index = i;
      g_palette = kPalettes[i];
      Recompose();
      return true;
    }
  }
  return false;
}

const std::string& NextTheme() {
  g_index = (g_index + 1) % kPaletteCount;
  g_palette = kPalettes[g_index];
  Recompose();
  return g_palette.label;
}

bool OverrideColor(const std::string& role, Rgb value) {
  // Named rather than offset-addressed so a config key is checked against the
  // same spelling the palette uses, and an unknown one can be reported.
  static const struct {
    const char* name;
    Rgb Palette::*field;
  } kRoles[] = {
      {"bg", &Palette::bg},
      {"surface", &Palette::surface},
      {"surface_alt", &Palette::surface_alt},
      {"surface_raised", &Palette::surface_raised},
      {"border", &Palette::border},
      {"text", &Palette::text},
      {"text_dim", &Palette::text_dim},
      {"text_faint", &Palette::text_faint},
      {"accent", &Palette::accent},
      {"green", &Palette::green},
      {"yellow", &Palette::yellow},
      {"blue", &Palette::blue},
      {"red", &Palette::red},
      {"purple", &Palette::purple},
      {"cyan", &Palette::cyan},
  };
  for (const auto& entry : kRoles) {
    if (role == entry.name) {
      g_palette.*(entry.field) = value;
      // A palette with a color changed is no longer the built-in one, and
      // saying so beats a header that claims to be Nord while showing
      // somebody's own blue.
      if (g_palette.label.find(" (custom)") == std::string::npos) {
        g_palette.label += " (custom)";
      }
      Recompose();
      return true;
    }
  }
  return false;
}

ColorDepth DetectColorDepth() {
  // NO_COLOR is a promise: set at all, whatever the value, means no color.
  // https://no-color.org
  if (std::getenv("NO_COLOR") != nullptr) {
    return ColorDepth::None;
  }
  const char* term = std::getenv("TERM");
  if (term == nullptr || std::strcmp(term, "dumb") == 0 || term[0] == '\0') {
    return ColorDepth::None;
  }
  const char* colorterm = std::getenv("COLORTERM");
  if (Contains(colorterm, "truecolor") || Contains(colorterm, "24bit")) {
    return ColorDepth::TrueColor;
  }
  // Windows Terminal, which is how a WSL shell usually reaches a screen. It
  // sets no COLORTERM and reports TERM=xterm-256color, so every check below
  // reads it as an eight-bit terminal — and it has been 24-bit since it
  // shipped. Without this gittop's own detection is worse than the one FTXUI
  // does downstream, which is a strange thing for the layer that owns the
  // palettes to be.
  const char* wt = std::getenv("WT_SESSION");
  if (wt != nullptr && wt[0] != '\0') {
    return ColorDepth::TrueColor;
  }
  // A `-direct` terminfo entry *is* the 24-bit one — xterm-direct, tmux-direct.
  // This used to be tested alongside "256color" and answered Ansi256, which
  // downgraded the one class of terminal that had said outright it could do
  // better. It has to be tried before the 256 test, since several of these
  // names contain both words.
  if (Contains(term, "direct")) {
    return ColorDepth::TrueColor;
  }
  // These three set no COLORTERM under some launchers but have never shipped a
  // version without 24-bit colour, and guessing low is not free: two palettes
  // that differ in 24-bit can quantize onto the same 256 index, which makes
  // switching theme look broken rather than subtle.
  if (Contains(term, "kitty") || Contains(term, "alacritty") || Contains(term, "wezterm")) {
    return ColorDepth::TrueColor;
  }
  if (Contains(term, "256color")) {
    return ColorDepth::Ansi256;
  }
  return ColorDepth::Ansi16;
}

ColorDepth ColorDepthNow() {
  return g_depth;
}

void SetColorDepth(ColorDepth depth) {
  g_depth = depth;

  // FTXUI quantizes a second time on its way to the screen, from its own
  // reading of TERM and COLORTERM, and it does not consult ToColor's. Leaving
  // the two to disagree is what made `theme.depth = "truecolor"` a setting that
  // appeared to do nothing: gittop stopped quantizing and FTXUI carried on. The
  // depth resolved here is the one decision, so it has to reach both.
  using Support = ftxui::Terminal::Color;
  switch (depth) {
    case ColorDepth::None:
      // FTXUI has no monochrome level. Palette16 is the floor; ToColor has
      // already mapped every role to Color::Default, so nothing colored is
      // emitted regardless of what FTXUI would allow.
      ftxui::Terminal::SetColorSupport(Support::Palette16);
      break;
    case ColorDepth::Ansi16:
      ftxui::Terminal::SetColorSupport(Support::Palette16);
      break;
    case ColorDepth::Ansi256:
      ftxui::Terminal::SetColorSupport(Support::Palette256);
      break;
    case ColorDepth::TrueColor:
      ftxui::Terminal::SetColorSupport(Support::TrueColor);
      break;
  }
}

std::string ColorDepthName(ColorDepth depth) {
  switch (depth) {
    case ColorDepth::None:
      return "monochrome";
    case ColorDepth::Ansi16:
      return "16 colors";
    case ColorDepth::Ansi256:
      return "256 colors";
    case ColorDepth::TrueColor:
      break;
  }
  return "truecolor";
}

std::string ColorDepthKey(ColorDepth depth) {
  switch (depth) {
    case ColorDepth::None:
      return "none";
    case ColorDepth::Ansi16:
      return "16";
    case ColorDepth::Ansi256:
      return "256";
    case ColorDepth::TrueColor:
      break;
  }
  return "truecolor";
}

bool ParseColorDepth(const std::string& text, ColorDepth* out) {
  const std::string key = Normalize(text);
  if (key == "auto") {
    *out = DetectColorDepth();
    return true;
  }
  if (key == "truecolor" || key == "24bit" || key == "rgb") {
    *out = ColorDepth::TrueColor;
    return true;
  }
  if (key == "256" || key == "256color") {
    *out = ColorDepth::Ansi256;
    return true;
  }
  if (key == "16" || key == "16color" || key == "ansi") {
    *out = ColorDepth::Ansi16;
    return true;
  }
  if (key == "none" || key == "mono" || key == "monochrome" || key == "off") {
    *out = ColorDepth::None;
    return true;
  }
  return false;
}

}  // namespace gittop::ui
