#include "ui/diff_panel.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "ui/theme.hpp"
#include "ui/widgets.hpp"

namespace gittop::ui {
namespace {

using model::DiffLine;
using model::DiffLineKind;
using model::DiffSnapshot;
using model::DiffSource;

using namespace ftxui;  // NOLINT: the dom DSL reads badly when qualified.

// Rendering every line of a ten-thousand-line diff builds ten thousand dom
// nodes to show forty of them. The window is sliced around the cursor instead,
// which is why the panel needs to know its own height.
constexpr int kOverscan = 40;

std::string ChangeWord(model::Change change) {
  switch (change) {
    case model::Change::Added:
      return "added";
    case model::Change::Deleted:
      return "deleted";
    case model::Change::Renamed:
      return "renamed";
    case model::Change::TypeChange:
      return "type changed";
    case model::Change::Untracked:
      return "new";
    case model::Change::Modified:
    case model::Change::None:
      break;
  }
  return "modified";
}

std::string SourceWord(DiffSource source) {
  switch (source) {
    case DiffSource::Staged:
      return "staged";
    case DiffSource::Commit:
      return "commit";
    case DiffSource::Worktree:
      break;
  }
  return "unstaged";
}

// Right-aligned, blank where the line does not exist on that side. A zero there
// would read as line zero rather than as "this line is not on this side".
std::string Number(int value, int width) {
  std::string text = value > 0 ? std::to_string(value) : std::string();
  if (static_cast<int>(text.size()) >= width) {
    return text;
  }
  return std::string(static_cast<std::size_t>(width) - text.size(), ' ') + text;
}

// How wide the two number columns have to be. Measured from the largest number
// actually present rather than fixed, so a short file does not pay eight cells
// of gutter for numbers that never reach three digits.
int GutterWidth(const DiffSnapshot& diff) {
  int largest = 0;
  for (const DiffLine& line : diff.lines) {
    largest = std::max({largest, line.old_lineno, line.new_lineno});
  }
  int digits = 1;
  while (largest >= 10) {
    largest /= 10;
    ++digits;
  }
  return std::max(2, digits);
}

Element FileBanner(const DiffSnapshot& diff, const std::string& path, bool selected) {
  const Theme& t = theme();

  // The header line carries the path; the counts come from the file record that
  // owns it, found by path because that is what the line stores.
  std::size_t additions = 0;
  std::size_t deletions = 0;
  std::string change = "modified";
  std::string old_path;
  for (const model::DiffFile& file : diff.files) {
    if (file.path == path) {
      additions = file.additions;
      deletions = file.deletions;
      change = ChangeWord(file.change);
      old_path = file.old_path;
      break;
    }
  }

  Elements parts{
      text(selected ? "▌" : " ") | color(t.accent),
      text(" "),
  };
  if (!old_path.empty()) {
    parts.push_back(PathText(old_path, false));
    parts.push_back(text("  →  ") | color(t.text_faint));
  }
  parts.push_back(PathText(path, true));
  parts.push_back(text("  "));
  parts.push_back(text(change) | color(t.text_faint));
  parts.push_back(filler());
  if (additions > 0) {
    parts.push_back(text("+" + std::to_string(additions) + " ") | color(t.diff_add));
  }
  if (deletions > 0) {
    parts.push_back(text("−" + std::to_string(deletions) + " ") | color(t.diff_del));
  }

  return hbox(std::move(parts)) | bold | bgcolor(t.surface_raised);
}

Element LineRow(const DiffSnapshot& diff, std::size_t index, bool selected, int gutter) {
  const Theme& t = theme();
  const DiffLine& line = diff.lines[index];

  if (line.kind == DiffLineKind::FileHeader) {
    return FileBanner(diff, line.text, selected);
  }

  if (line.kind == DiffLineKind::HunkHeader) {
    return hbox({
               text(selected ? "▌" : " ") | color(t.accent),
               text(" " + line.text) | color(t.diff_hunk),
               filler(),
           }) |
           bgcolor(t.surface_alt);
  }

  if (line.kind == DiffLineKind::Binary || line.kind == DiffLineKind::Note) {
    return hbox({
        text(selected ? "▌" : " ") | color(t.accent),
        text("  " + line.text) | color(t.text_faint) | dim,
        filler(),
    });
  }

  // Sign and colour both, because a diff read with no colour at all — the
  // sixteen-colour fallback, or NO_COLOR — is still a diff someone has to be
  // able to trust, and the sign is the part git itself relies on.
  std::string sign = " ";
  Swatch fill = t.text_dim;
  Swatch tint = t.surface;
  if (line.kind == DiffLineKind::Added) {
    sign = "+";
    fill = t.diff_add;
    tint = t.diff_add_bg;
  } else if (line.kind == DiffLineKind::Removed) {
    sign = "-";
    fill = t.diff_del;
    tint = t.diff_del_bg;
  }

  Elements parts{text(selected ? "▌" : " ") | color(t.accent)};
  if (gutter > 0) {
    parts.push_back(text(Number(line.old_lineno, gutter) + " " +
                         Number(line.new_lineno, gutter) + " ") |
                    color(t.text_faint));
  }
  parts.push_back(text(sign) | bold | color(fill));
  parts.push_back(text(line.text) | color(line.kind == DiffLineKind::Context ? t.text_dim : fill));
  parts.push_back(filler());

  Element row = hbox(std::move(parts)) | bgcolor(selected ? t.surface_alt : tint);
  return row;
}

Element EmptyState(const DiffSnapshot& diff) {
  const Theme& t = theme();
  std::string headline = "no unstaged changes";
  std::string hint = "edit something, or press s for the staged diff";
  if (diff.source == DiffSource::Staged) {
    headline = "nothing staged";
    hint = "stage a file on the status view, or press s for the unstaged diff";
  } else if (diff.source == DiffSource::Commit) {
    headline = "this commit changed nothing";
    hint = "an empty commit, or a merge with no conflicts to record";
  }
  return vbox({
      filler(),
      hbox({filler(), text("≡") | bold | color(t.text_faint), filler()}),
      text(""),
      hbox({filler(), text(headline) | color(t.text), filler()}),
      hbox({filler(), text(hint) | color(t.text_faint), filler()}),
      filler(),
  });
}

}  // namespace

Element DiffPanel(const DiffSnapshot& diff, const DiffView& view, int width, int height) {
  const Theme& t = theme();

  Elements header{
      text(" " + SourceWord(diff.source) + " ") | bold | color(t.bg) | bgcolor(t.accent),
      text("  "),
      text(diff.title) | bold | color(t.text),
  };
  if (!diff.subtitle.empty() && diff.subtitle != diff.title) {
    header.push_back(text("  "));
    header.push_back(text(diff.subtitle) | color(t.text_faint));
  }
  header.push_back(filler());
  if (!diff.files.empty()) {
    header.push_back(text(std::to_string(diff.files.size()) +
                          (diff.files.size() == 1 ? " file  " : " files  ")) |
                     color(t.text_faint));
    header.push_back(text("+" + std::to_string(diff.additions) + " ") | color(t.diff_add));
    header.push_back(text("−" + std::to_string(diff.deletions) + " ") | color(t.diff_del));
  }

  Element title = hbox(std::move(header)) | bgcolor(t.surface);

  if (!diff.error.empty()) {
    return window(text(" DIFF ") | bold | color(t.text_dim),
                  vbox({
                      title,
                      separator() | color(t.border),
                      filler(),
                      hbox({filler(), text("✗") | bold | color(t.danger), filler()}),
                      text(""),
                      hbox({filler(), text(diff.error) | color(t.text), filler()}),
                      filler(),
                  })) |
           color(t.border) | bgcolor(t.surface) | flex;
  }

  if (diff.lines.empty()) {
    return window(text(" DIFF ") | bold | color(t.text_dim),
                  vbox({title, separator() | color(t.border), EmptyState(diff) | flex})) |
           color(t.border) | bgcolor(t.surface) | flex;
  }

  // The slice is centred on the cursor and padded by a screen either side, so
  // yframe still has somewhere to scroll to before the next redraw catches up.
  const auto total = static_cast<int>(diff.lines.size());
  const int rows = std::max(4, height);
  const int selected = std::clamp(view.selected, 0, total - 1);
  const int first = std::max(0, selected - rows - kOverscan);
  const int last = std::min(total, selected + rows + kOverscan);

  const int gutter = view.line_numbers && width >= 70 ? GutterWidth(diff) : 0;

  Elements body;
  body.reserve(static_cast<std::size_t>(last - first) + 2);
  if (first > 0) {
    body.push_back(hbox({text("   "), text("… " + std::to_string(first) + " lines above") |
                                          color(t.text_faint)}));
  }
  for (int i = first; i < last; ++i) {
    Element row = LineRow(diff, static_cast<std::size_t>(i), i == selected, gutter);
    if (i == selected) {
      row = std::move(row) | ftxui::focus;
    }
    body.push_back(std::move(row));
  }
  if (last < total) {
    body.push_back(hbox({text("   "), text("… " + std::to_string(total - last) + " lines below") |
                                          color(t.text_faint)}));
  }

  return window(text(" DIFF ") | bold | color(t.text_dim),
                vbox({
                    title,
                    separator() | color(t.border),
                    vbox(std::move(body)) | vscroll_indicator | yframe | flex,
                })) |
         color(t.border) | bgcolor(t.surface) | flex;
}

}  // namespace gittop::ui
