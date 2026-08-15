#include "ui/widgets.hpp"

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>

#include "ui/glyphs.hpp"

namespace gittop::ui {
namespace {

using namespace ftxui;  // NOLINT: the dom DSL reads badly when qualified.

class GradientBarNode : public Node {
 public:
  GradientBarNode(float ratio, Ramp ramp, Rgb trough)
      : ratio_(std::clamp(ratio, 0.0F, 1.0F)), ramp_(ramp), trough_(trough) {}

  void ComputeRequirement() override {
    requirement_.min_x = 1;
    requirement_.min_y = 1;
    requirement_.flex_grow_x = 1;
  }

  void Render(Screen& screen) override {
    const int width = box_.x_max - box_.x_min + 1;
    if (width <= 0 || box_.y_max < box_.y_min) {
      return;
    }
    const int y = box_.y_min;
    const GlyphSet& g = glyphs();

    const float filled = ratio_ * static_cast<float>(width);
    const int full_cells = static_cast<int>(filled);
    const int eighths = static_cast<int>((filled - static_cast<float>(full_cells)) * 8.0F);

    for (int x = 0; x < width; ++x) {
      Cell& cell = screen.PixelAt(box_.x_min + x, y);
      const float t =
          width > 1 ? static_cast<float>(x) / static_cast<float>(width - 1) : 0.0F;

      if (x < full_cells) {
        cell.character = g.bar_full;
        cell.foreground_color = ToColor(Mix(ramp_.from, ramp_.to, t));
      } else if (x == full_cells && eighths > 0) {
        cell.character = g.eighths[static_cast<std::size_t>(eighths) - 1];
        cell.foreground_color = ToColor(Mix(ramp_.from, ramp_.to, t));
      } else {
        // A track rather than empty space, so an all-zero row still reads as a
        // set of bars waiting for data instead of a blank panel.
        cell.character = g.bar_track;
        cell.foreground_color = ToColor(trough_);
      }
    }
  }

 private:
  float ratio_;
  Ramp ramp_;
  Rgb trough_;
};

PanelBorder g_border = PanelBorder::Rounded;
bool g_reduced_motion = false;
float g_overlay_reveal = 1.0F;

ftxui::BorderStyle ToFtxui(PanelBorder border) {
  // Heavy is the one style built from characters a non-UTF-8 terminal is likely
  // to be missing; light, rounded and double all live in the line-drawing block
  // that CP437 and the Linux console font both carry. So ASCII mode downgrades
  // heavy and leaves the other three alone rather than flattening all four.
  if (border == PanelBorder::Heavy && GlyphModeNow() == GlyphMode::Ascii) {
    return ftxui::LIGHT;
  }
  switch (border) {
    case PanelBorder::Light:
      return ftxui::LIGHT;
    case PanelBorder::Heavy:
      return ftxui::HEAVY;
    case PanelBorder::Double:
      return ftxui::DOUBLE;
    case PanelBorder::Rounded:
      break;
  }
  return ftxui::ROUNDED;
}

}  // namespace

// ---------------------------------------------------------------- measurement

int TextWidth(const std::string& text) { return string_width(text); }

std::string Truncate(const std::string& text, int cells) {
  if (cells <= 0) {
    return {};
  }
  if (string_width(text) <= cells) {
    return text;
  }

  // Utf8ToGlyphs returns one entry per *cell*: a double-width glyph is followed
  // by an empty string standing in for the column it also occupies. That is
  // exactly the invariant needed here — cutting at index n is safe unless entry
  // n is the empty half, which would mean entry n-1 is a wide glyph losing its
  // second column.
  const std::vector<std::string> cellwise = Utf8ToGlyphs(text);
  const std::string tail = glyphs().ellipsis;
  const int room = std::max(0, cells - string_width(tail));

  auto keep = static_cast<std::size_t>(room);
  keep = std::min(keep, cellwise.size());
  while (keep > 0 && keep < cellwise.size() && cellwise[keep].empty()) {
    --keep;
  }

  std::string out;
  for (std::size_t i = 0; i < keep; ++i) {
    out += cellwise[i];
  }
  out += tail;
  return out;
}

std::string Fit(const std::string& text, int cells) {
  if (cells <= 0) {
    return {};
  }
  std::string out = Truncate(text, cells);
  const int short_by = cells - string_width(out);
  if (short_by > 0) {
    out.append(static_cast<std::size_t>(short_by), ' ');
  }
  return out;
}

std::string Rjust(const std::string& text, int cells) {
  const int width = string_width(text);
  if (width >= cells) {
    return Truncate(text, cells);
  }
  return std::string(static_cast<std::size_t>(cells - width), ' ') + text;
}

// ---------------------------------------------------------------- panel frame

bool ParsePanelBorder(const std::string& text, PanelBorder* out) {
  if (text == "rounded") {
    *out = PanelBorder::Rounded;
    return true;
  }
  if (text == "light" || text == "single" || text == "thin") {
    *out = PanelBorder::Light;
    return true;
  }
  if (text == "heavy" || text == "bold" || text == "thick") {
    *out = PanelBorder::Heavy;
    return true;
  }
  if (text == "double") {
    *out = PanelBorder::Double;
    return true;
  }
  return false;
}

std::string PanelBorderName(PanelBorder border) {
  switch (border) {
    case PanelBorder::Light:
      return "light";
    case PanelBorder::Heavy:
      return "heavy";
    case PanelBorder::Double:
      return "double";
    case PanelBorder::Rounded:
      break;
  }
  return "rounded";
}

PanelBorder PanelBorderNow() { return g_border; }

void SetPanelBorder(PanelBorder border) { g_border = border; }

Element Panel(const std::string& title, Element content, PanelStyle style) {
  const Theme& t = theme();

  // Three ranks, and they are ordered: an alarm outranks focus, focus outranks
  // an idle sidecar. Without the ordering an errored panel that happens not to
  // hold the cursor is drawn in the quietest style on the screen.
  const Swatch edge = style.alarm ? t.danger : (style.focused ? t.border_focus : t.border);
  const Swatch ink = style.alarm ? t.danger : (style.focused ? t.accent : t.text_dim);

  Elements bar{text(" "), text(title) | bold | color(ink)};
  if (!style.note.empty()) {
    bar.push_back(text("  "));
    bar.push_back(text(style.note) | color(t.text_faint));
  }
  bar.push_back(text(" "));

  return window(hbox(std::move(bar)), std::move(content), ToFtxui(g_border)) | color(edge) |
         bgcolor(t.surface);
}

Decorator FramedBorder() { return borderStyled(ToFtxui(g_border)); }

Element Scrollable(Element list) {
  if (GlyphModeNow() == GlyphMode::Ascii) {
    return std::move(list) | yframe;
  }
  return std::move(list) | vscroll_indicator | yframe;
}

Element Gap(int cells) {
  return text(std::string(static_cast<std::size_t>(std::max(0, cells)), ' '));
}

// ----------------------------------------------------------------- indicators

Element GradientBar(float ratio, Ramp ramp, Swatch trough) {
  return std::make_shared<GradientBarNode>(ratio, ramp, trough.rgb);
}

Element Sparkline(const int* values, std::size_t count, int peak, Ramp ramp) {
  const Theme& t = theme();
  const GlyphSet& g = glyphs();

  Elements cells;
  cells.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    if (values[i] <= 0 || peak <= 0) {
      cells.push_back(text(g.bullet) | color(t.surface_alt));
      continue;
    }
    const int level = std::clamp((values[i] * 8) / peak, 1, 8);
    const float ratio = static_cast<float>(level) / 8.0F;
    cells.push_back(text(g.blocks[static_cast<std::size_t>(level) - 1]) |
                    color(ToColor(Mix(ramp.from, ramp.to, ratio))));
  }
  return hbox(std::move(cells));
}

Element Sparkline(const std::vector<int>& values, int peak, Ramp ramp) {
  return Sparkline(values.data(), values.size(), peak, ramp);
}

Element SkeletonRows(int rows, int frame) {
  const Theme& t = theme();

  // Widths that differ per row and repeat every seven, so the block reads as
  // text-shaped rather than as a progress bar someone forgot to fill.
  static constexpr std::array<int, 7> kWidths = {34, 52, 41, 60, 28, 47, 38};

  Elements out;
  out.reserve(static_cast<std::size_t>(std::max(0, rows)));
  for (int i = 0; i < rows; ++i) {
    const auto width = kWidths[static_cast<std::size_t>(i) % kWidths.size()];

    // A highlight travelling down the rows rather than across them. Across
    // would need a custom node to shade per cell; down needs one Mix per row
    // and reads the same at this size.
    float glow = 0.0F;
    if (!ReducedMotion()) {
      const float phase = static_cast<float>(((frame - (i * 2)) % 24 + 24) % 24) / 24.0F;
      glow = std::max(0.0F, 1.0F - (phase * 4.0F));
    }
    const Rgb tint = Mix(t.surface_raised.rgb, t.text_faint.rgb, glow * 0.5F);

    out.push_back(hbox({
        text("  "),
        text(std::string(static_cast<std::size_t>(width), ' ')) | bgcolor(ToColor(tint)),
        filler(),
    }));
  }
  return vbox(std::move(out));
}

Element Chip(const std::string& key, const std::string& label) {
  Elements parts;
  parts.push_back(text(" " + key + " ") | bold | color(theme().accent) |
                  bgcolor(theme().surface_raised));
  if (!label.empty()) {
    parts.push_back(text(" " + label) | color(theme().text_faint));
  }
  parts.push_back(text("  "));
  return hbox(std::move(parts));
}

Element PathText(const std::string& path, bool emphasised, int cells) {
  const auto slash = path.find_last_of('/');
  const Swatch name_color = emphasised ? theme().text : theme().text_dim;

  if (slash == std::string::npos) {
    Element name = text(cells > 0 ? Truncate(path, cells) : path) | color(name_color);
    if (emphasised) {
      name = name | bold;
    }
    return name;
  }

  std::string directory = path.substr(0, slash + 1);
  std::string name = path.substr(slash + 1);

  if (cells > 0) {
    // The filename is what the list is scanned by, so it survives and the
    // directory gives way. Only once the name alone will not fit does the name
    // itself get cut.
    const int name_width = TextWidth(name);
    if (name_width >= cells) {
      directory.clear();
      name = Truncate(name, cells);
    } else {
      directory = Truncate(directory, cells - name_width);
    }
  }

  Element leaf = text(name) | color(name_color);
  if (emphasised) {
    leaf = leaf | bold;
  }
  if (directory.empty()) {
    return leaf;
  }
  return hbox({text(directory) | color(theme().text_faint), std::move(leaf)});
}

// ---------------------------------------------------------------------- motion

bool ReducedMotion() { return g_reduced_motion; }

void SetReducedMotion(bool reduced) { g_reduced_motion = reduced; }

float OverlayReveal() { return g_overlay_reveal; }

void SetOverlayReveal(float reveal) { g_overlay_reveal = std::clamp(reveal, 0.0F, 1.0F); }

Swatch Emerging(Swatch swatch) {
  if (g_overlay_reveal >= 0.999F) {
    return swatch;
  }
  return Swatch{Mix(theme().bg.rgb, swatch.rgb, g_overlay_reveal)};
}

float Pulse(int frame) {
  if (ReducedMotion()) {
    return 1.0F;
  }
  // Twenty frames a cycle against the spinner's 90ms step is a hair under two
  // seconds, which is slow enough to read as breathing rather than flashing.
  constexpr int kPeriod = 20;
  const int phase = ((frame % kPeriod) + kPeriod) % kPeriod;
  return 0.5F - (0.5F * std::cos((static_cast<float>(phase) / kPeriod) * 6.2831853F));
}

std::string SpinnerFrame(int frame) {
  const GlyphSet& g = glyphs();
  if (ReducedMotion()) {
    // Still a mark, and still the one the moving spinner passes through, so a
    // running row is distinguishable from a finished one without any motion.
    return g.spinner[0];
  }
  const auto count = static_cast<int>(g.spinner.size());
  const int index = ((frame % count) + count) % count;
  return g.spinner[static_cast<std::size_t>(index)];
}

std::string RelativeTime(std::int64_t when, std::int64_t now) {
  const std::int64_t delta = now - when;
  if (delta < 60) {
    return "now";
  }
  if (delta < 3600) {
    return std::to_string(delta / 60) + "m";
  }
  if (delta < 86400) {
    return std::to_string(delta / 3600) + "h";
  }
  if (delta < 86400LL * 30) {
    return std::to_string(delta / 86400) + "d";
  }
  if (delta < 86400LL * 365) {
    return std::to_string(delta / (86400LL * 30)) + "mo";
  }
  return std::to_string(delta / (86400LL * 365)) + "y";
}

// A remote can be configured as https://user:token@host/path. That token must
// never reach the screen, so the userinfo is replaced rather than shortened.
std::string SafeUrl(const std::string& url) {
  const std::size_t scheme = url.find("://");
  if (scheme == std::string::npos) {
    return url;  // scp-style user@host:path carries no password
  }
  const std::size_t start = scheme + 3;
  const std::size_t slash = url.find('/', start);
  const std::size_t authority_end = slash == std::string::npos ? url.size() : slash;
  const std::size_t at = url.rfind('@', authority_end);

  if (at == std::string::npos || at < start) {
    return url;
  }
  return url.substr(0, start) + glyphs().mask + "@" + url.substr(at + 1);
}

}  // namespace gittop::ui
