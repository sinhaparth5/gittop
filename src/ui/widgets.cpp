#include "ui/widgets.hpp"

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <utility>

namespace gittop::ui {
namespace {

using namespace ftxui;  // NOLINT: the dom DSL reads badly when qualified.

constexpr std::array<const char*, 8> kEighths = {"▏", "▎", "▍", "▌", "▋", "▊", "▉", "█"};

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

    const float filled = ratio_ * static_cast<float>(width);
    const int full_cells = static_cast<int>(filled);
    const int eighths = static_cast<int>((filled - static_cast<float>(full_cells)) * 8.0F);

    for (int x = 0; x < width; ++x) {
      Cell& cell = screen.PixelAt(box_.x_min + x, y);
      const float t =
          width > 1 ? static_cast<float>(x) / static_cast<float>(width - 1) : 0.0F;

      if (x < full_cells) {
        cell.character = "█";
        cell.foreground_color = ToColor(Mix(ramp_.from, ramp_.to, t));
      } else if (x == full_cells && eighths > 0) {
        cell.character = kEighths[static_cast<std::size_t>(eighths) - 1];
        cell.foreground_color = ToColor(Mix(ramp_.from, ramp_.to, t));
      } else {
        // A track rather than empty space, so an all-zero row still reads as a
        // set of bars waiting for data instead of a blank panel.
        cell.character = "─";
        cell.foreground_color = ToColor(trough_);
      }
    }
  }

 private:
  float ratio_;
  Ramp ramp_;
  Rgb trough_;
};

}  // namespace

Element GradientBar(float ratio, Ramp ramp, Swatch trough) {
  return std::make_shared<GradientBarNode>(ratio, ramp, trough.rgb);
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

Element PathText(const std::string& path, bool emphasised) {
  const auto slash = path.find_last_of('/');
  const Swatch name_color = emphasised ? theme().text : theme().text_dim;

  if (slash == std::string::npos) {
    Element name = text(path) | color(name_color);
    if (emphasised) {
      name = name | bold;
    }
    return name;
  }

  Element name = text(path.substr(slash + 1)) | color(name_color);
  if (emphasised) {
    name = name | bold;
  }
  return hbox({
      text(path.substr(0, slash + 1)) | color(theme().text_faint),
      std::move(name),
  });
}

}  // namespace gittop::ui
