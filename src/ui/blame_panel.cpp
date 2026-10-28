#include "ui/blame_panel.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <utility>

#include "ui/glyphs.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

namespace gittop::ui {

using namespace ftxui;  // NOLINT: the dom DSL reads badly when qualified.

Element BlamePanel(const model::BlameSnapshot& blame, bool loading, int selected, int width,
                   int rows) {
  const Theme& t = theme();
  const std::string title = "BLAME " + blame.path;

  if (!blame.error.empty()) {
    return Panel(title, text(" " + blame.error) | color(t.danger), {.alarm = true}) | flex;
  }
  if (blame.lines.empty()) {
    return Panel(title, text(loading ? std::string(" reading history") + glyphs().ellipsis : " no lines") |
                            color(t.text_faint)) |
           flex;
  }

  const int count = static_cast<int>(blame.lines.size());
  const int start = std::clamp(selected - rows / 2, 0, std::max(0, count - rows));
  const int end = std::min(count, start + rows);
  const int digits = static_cast<int>(std::to_string(count).size());
  const auto now = static_cast<std::int64_t>(std::time(nullptr));
  // id, author and age on the first line of a run; the rest of the gutter is
  // blank so a commit reads as one block rather than a column of repeats.
  const int author_cells = std::clamp(width / 8, 6, 16);

  Elements lines;
  for (int i = start; i < end; ++i) {
    const model::BlameLine& line = blame.lines[static_cast<std::size_t>(i)];
    const bool is_selected = i == selected;
    const bool head = line.first || i == start;

    Element gutter =
        head ? hbox({
                   text(line.id.substr(0, 7)) | color(t.accent),
                   text(" "),
                   text(Fit(line.author, author_cells)) | color(t.text_dim),
                   text(" "),
                   text(Rjust(line.time > 0 ? RelativeTime(line.time, now) : "", 4)) |
                       color(t.text_faint),
               })
             : text(std::string(static_cast<std::size_t>(7 + 1 + author_cells + 1 + 4), ' '));

    Element row = hbox({
        text(is_selected ? glyphs().cursor : " ") | color(t.accent),
        text(" "),
        std::move(gutter),
        text(" " + Rjust(std::to_string(line.lineno), digits) + " ") | color(t.text_faint),
        text(line.text) | color(is_selected ? t.text : t.text_dim),
    });
    if (is_selected) {
      row = std::move(row) | bgcolor(t.surface_alt);
    }
    lines.push_back(std::move(row));
  }

  const model::BlameLine& here =
      blame.lines[static_cast<std::size_t>(std::clamp(selected, 0, count - 1))];
  return Panel(title, vbox(std::move(lines)) | flex, {.note = here.summary}) | flex;
}

}  // namespace gittop::ui
