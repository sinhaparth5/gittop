#include "ui/theme.hpp"

#include <algorithm>
#include <cmath>

namespace gittop::ui {
namespace {

constexpr Swatch S(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
  return Swatch{Rgb{r, g, b}};
}

constexpr Ramp R(std::uint8_t r0, std::uint8_t g0, std::uint8_t b0, std::uint8_t r1,
                 std::uint8_t g1, std::uint8_t b1) {
  return Ramp{Rgb{r0, g0, b0}, Rgb{r1, g1, b1}};
}

// Deep, slightly blue ground with three surface steps above it. Three steps is
// enough to separate panel from selection from chip without any of them reading
// as a different color.
const Theme kDefault{
    .bg = S(0x0b, 0x0f, 0x16),
    .surface = S(0x13, 0x19, 0x24),
    .surface_alt = S(0x1e, 0x26, 0x36),
    .surface_raised = S(0x27, 0x31, 0x45),

    .border = S(0x26, 0x30, 0x43),
    .border_focus = S(0xf0, 0x88, 0x3e),

    .text = S(0xd7, 0xde, 0xe8),
    .text_dim = S(0x97, 0xa3, 0xb6),
    .text_faint = S(0x5e, 0x6b, 0x7f),

    .accent = S(0xf0, 0x88, 0x3e),

    .staged = S(0x56, 0xd3, 0x64),
    .unstaged = S(0xe3, 0xb3, 0x41),
    .untracked = S(0x79, 0xc0, 0xff),
    .conflict = S(0xff, 0x7b, 0x72),

    .success = S(0x56, 0xd3, 0x64),
    .warning = S(0xe3, 0xb3, 0x41),
    .danger = S(0xff, 0x7b, 0x72),

    .staged_ramp = R(0x23, 0x8b, 0x3a, 0x6b, 0xe5, 0x78),
    .unstaged_ramp = R(0xa8, 0x71, 0x08, 0xf2, 0xc5, 0x5c),
    .untracked_ramp = R(0x28, 0x5f, 0xb8, 0x8f, 0xcc, 0xff),
    .conflict_ramp = R(0xb8, 0x33, 0x2e, 0xff, 0x94, 0x8c),
};

}  // namespace

ftxui::Color ToColor(Rgb c) {
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

const Theme& theme() {
  return kDefault;
}

}  // namespace gittop::ui
