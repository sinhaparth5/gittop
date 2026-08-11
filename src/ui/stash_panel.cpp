#include "ui/stash_panel.hpp"

#include <cstddef>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "ui/theme.hpp"
#include "ui/widgets.hpp"

namespace gittop::ui {
namespace {

using namespace ftxui;  // NOLINT: the dom DSL reads badly when qualified.

Element EmptyState() {
  const Theme& t = theme();
  return vbox({
      filler(),
      hbox({filler(), text("⊘") | bold | color(t.text_faint), filler()}),
      text(""),
      hbox({filler(), text("no stashes") | color(t.text), filler()}),
      hbox({filler(),
            text("S puts the working tree away and gives you a clean one") | color(t.text_faint),
            filler()}),
      filler(),
  });
}

}  // namespace

Element StashList(const model::StashList& stashes, int selected, std::vector<Box>* row_boxes) {
  const Theme& t = theme();

  if (stashes.empty()) {
    if (row_boxes != nullptr) {
      row_boxes->clear();
    }
    return window(text(" STASHES ") | bold | color(t.text_dim), EmptyState()) | color(t.border) |
           bgcolor(t.surface) | flex;
  }

  if (row_boxes != nullptr) {
    row_boxes->assign(stashes.entries.size(), Box());
  }
  const auto now = static_cast<std::int64_t>(std::time(nullptr));

  Elements rows;
  rows.reserve(stashes.entries.size());

  for (std::size_t i = 0; i < stashes.entries.size(); ++i) {
    const model::Stash& stash = stashes.entries[i];
    const bool is_selected = static_cast<int>(i) == selected;

    Elements parts{
        text(is_selected ? "▌" : " ") | color(t.accent),
        text(" "),
        // The index is the handle every stash command takes, so it is printed
        // rather than left to be counted off the screen.
        text("stash@{" + std::to_string(stash.index) + "}") | color(t.accent),
        text("  "),
    };
    if (!stash.branch.empty()) {
      parts.push_back(text(" " + stash.branch + " ") | color(t.bg) | bgcolor(t.untracked));
      parts.push_back(text("  "));
    }
    parts.push_back(text(stash.summary) | color(is_selected ? t.text : t.text_dim));
    parts.push_back(filler());
    parts.push_back(text("  "));
    parts.push_back(text(stash.short_id) | color(t.text_faint));
    parts.push_back(text("  "));
    parts.push_back(text(stash.time > 0 ? RelativeTime(stash.time, now) : "") |
                    color(t.text_faint) | size(WIDTH, EQUAL, 4));
    parts.push_back(text(" "));

    Element row = hbox(std::move(parts));
    if (is_selected) {
      row = std::move(row) | bgcolor(t.surface_alt) | focus;
    }
    if (row_boxes != nullptr) {
      row = std::move(row) | reflect((*row_boxes)[i]);
    }
    rows.push_back(std::move(row));
  }

  return window(text(" STASHES ") | bold | color(t.text_dim),
                vbox(std::move(rows)) | vscroll_indicator | yframe) |
         color(t.border) | bgcolor(t.surface) | flex;
}

}  // namespace gittop::ui
