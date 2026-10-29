#include "ui/conflict_panel.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "ui/glyphs.hpp"
#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

namespace gittop::ui {
namespace {

using namespace ftxui;  // NOLINT: the dom DSL reads badly when qualified.

// Lines of the file shown above and below a block, faint, so the choice is
// made knowing what it sits between.
constexpr std::size_t kContext = 2;

Element Column(const std::string& title, const std::string& label, Color tint,
               const std::vector<std::string>& lines, int cells, int rows) {
  const Theme& t = theme();
  Elements out{hbox({
      text(" " + title + " ") | bold | inverted | color(tint),
      text(" " + Truncate(label, std::max(4, cells - static_cast<int>(title.size()) - 4))) |
          color(t.text_dim),
      filler(),
  })};
  if (lines.empty()) {
    // An empty side is an answer, not missing data: taking it deletes the lines
    // the other side has. Said in words so it is not read as a blank pane.
    out.push_back(text(" (nothing: this side removes these lines)") | color(t.text_faint));
  }
  const std::size_t shown = std::min(lines.size(), static_cast<std::size_t>(std::max(1, rows)));
  for (std::size_t i = 0; i < shown; ++i) {
    out.push_back(hbox({text(" "), text(Truncate(lines[i], cells - 1)) | color(t.text)}));
  }
  if (shown < lines.size()) {
    out.push_back(text(" " + std::string(glyphs().ellipsis) + " " +
                       std::to_string(lines.size() - shown) + " more lines") |
                  color(t.text_faint));
  }
  return vbox(std::move(out)) | size(WIDTH, EQUAL, cells);
}

Element Context(const model::ConflictFile& file, std::size_t from, std::size_t to, int cells) {
  const Theme& t = theme();
  Elements out;
  for (std::size_t i = from; i < to && i < file.lines.size(); ++i) {
    out.push_back(hbox({
        text(Fit(std::to_string(i + 1), 5)) | color(t.text_faint),
        text(Truncate(file.lines[i], cells - 6)) | color(t.text_faint),
    }));
  }
  return vbox(std::move(out));
}

}  // namespace

Element ConflictPane(const ConflictView& view, int width, int height) {
  const Theme& t = theme();
  const GlyphSet& g = glyphs();
  const model::ConflictFile& file = *view.file;
  const int pane = std::clamp(width - 8, 60, 140);
  const int column = (pane - 5) / 2;
  // Room left for the two columns once the frame, header, context and footer
  // have theirs.
  const int rows = std::max(3, height - 22);

  Elements body;
  body.push_back(hbox({
      text(" Resolve ") | bold | color(t.accent),
      text(Truncate(file.path, pane - 40)) | bold | color(t.text),
      filler(),
      text("file " + std::to_string(view.file_index + 1) + " of " +
           std::to_string(view.file_count)) |
          color(t.text_dim),
      text(view.operation.empty() ? " " : "  " + std::string(g.bullet) + "  " + view.operation + " ") |
          color(t.warning),
  }));
  body.push_back(separator() | color(t.border));

  const bool has_hunk = !file.hunks.empty();
  if (has_hunk) {
    const std::size_t index =
        static_cast<std::size_t>(std::clamp(view.hunk, 0, static_cast<int>(file.hunks.size()) - 1));
    const model::ConflictHunk& hunk = file.hunks[index];
    body.push_back(hbox({
        text(" block " + std::to_string(index + 1) + " of " + std::to_string(file.hunks.size())) |
            bold | color(t.conflict),
        text("  at line " + std::to_string(hunk.begin + 1)) | color(t.text_dim),
        filler(),
        text(hunk.base.empty() ? ""
                               : std::to_string(hunk.base.size()) +
                                     (hunk.base.size() == 1 ? " line" : " lines") +
                                     " in the common ancestor ") |
            color(t.text_faint),
    }));
    body.push_back(Context(file, hunk.begin >= kContext ? hunk.begin - kContext : 0, hunk.begin, pane));
    body.push_back(hbox({
        Column("OURS", hunk.ours_label, t.staged, hunk.ours, column, rows),
        separator() | color(t.border),
        Column("THEIRS", hunk.theirs_label, t.unstaged, hunk.theirs, column, rows),
    }));
    body.push_back(Context(file, hunk.end + 1, hunk.end + 1 + kContext, pane));
  } else {
    std::string why;
    if (file.binary) {
      why = "This is a binary file, so it cannot be merged a block at a time.";
    } else if (!file.ours_exists || !file.theirs_exists) {
      why = std::string("Deleted on ") + (file.ours_exists ? "their" : "our") +
            " side and changed on the other. Take one side whole.";
    } else if (file.unclosed) {
      why = "A <<<<<<< marker has no >>>>>>> after it. Finish the edit in the editor.";
    } else {
      why = "No conflict markers are left in it. Stage it to mark it resolved.";
    }
    body.push_back(text(""));
    body.push_back(hbox({text(std::string(" ") + g.warning + " ") | color(t.warning),
                         paragraph(why) | color(t.text)}));
    body.push_back(text(""));
  }

  // Rebase swaps the words: git replays your commit onto the other branch, so
  // "ours" is the branch being rebased onto and "theirs" is your own work.
  if (view.operation == "rebase") {
    body.push_back(text(" During a rebase, ours is the branch you are rebasing onto and theirs is "
                        "your commit.") |
                   color(t.text_faint));
  }

  body.push_back(separator() | color(t.border));
  Elements chips{text(" ")};
  if (has_hunk) {
    chips.push_back(Chip("o", "ours"));
    chips.push_back(Chip("t", "theirs"));
    chips.push_back(Chip("b", "both"));
  }
  chips.push_back(Chip("O/T", "whole file"));
  chips.push_back(Chip("e", view.editor.empty() ? "editor" : view.editor));
  if (!has_hunk && !file.unclosed && !file.binary && file.ours_exists && file.theirs_exists) {
    chips.push_back(Chip("s", "mark resolved"));
  }
  if (file.hunks.size() > 1) {
    chips.push_back(Chip("j/k", "block"));
  }
  if (view.file_count > 1) {
    chips.push_back(Chip("[/]", "file"));
  }
  chips.push_back(filler());
  chips.push_back(Chip("esc", "close"));
  body.push_back(hbox(std::move(chips)));

  return vbox(std::move(body)) | PaneFrame() | size(WIDTH, EQUAL, pane);
}

}  // namespace gittop::ui
