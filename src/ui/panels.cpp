#include "ui/panels.hpp"

#include <cstddef>
#include <vector>

#include "ui/theme.hpp"

namespace gittop::ui {
namespace {

using ftxui::Element;
using ftxui::Elements;
using model::Change;
using model::Stage;
using model::StatusEntry;

using namespace ftxui;  // NOLINT: the dom DSL reads badly when qualified.

std::string ChangeLetter(Change c) {
  switch (c) {
    case Change::Added:
      return "A";
    case Change::Modified:
      return "M";
    case Change::Deleted:
      return "D";
    case Change::Renamed:
      return "R";
    case Change::TypeChange:
      return "T";
    case Change::Untracked:
      return "?";
    case Change::None:
      break;
  }
  return " ";
}

struct Decoration {
  std::string glyph;
  std::string letter;
  Color color;
};

// Filled marker means staged, hollow means not. The letter carries the change
// type, so the row stays readable without color: a filled circle and an "M"
// tell the same story to someone who cannot tell green from amber.
Decoration Decorate(const StatusEntry& e) {
  const Theme& t = theme();
  switch (e.stage) {
    case Stage::Conflict:
      return {"!", "U", t.conflict};
    case Stage::Index:
      return {"●", ChangeLetter(e.change), t.staged};
    case Stage::Worktree:
      if (e.change == Change::Untracked) {
        return {"○", "?", t.untracked};
      }
      return {"○", ChangeLetter(e.change), t.unstaged};
  }
  return {"·", " ", t.text_dim};
}

Element Card(const std::string& label, std::size_t value, std::size_t total, Color accent) {
  const float ratio =
      total > 0 ? static_cast<float>(value) / static_cast<float>(total) : 0.0F;
  return vbox({
             hbox({
                 text(label) | color(theme().text_dim),
                 filler(),
                 text(std::to_string(value)) | bold | color(accent),
             }),
             gauge(ratio) | color(accent) | bgcolor(theme().surface_alt),
         }) |
         flex;
}

Element Hint(const std::string& text_value) {
  return text(text_value + "  ") | color(theme().text_faint);
}

Element Row(const StatusEntry& entry, bool selected) {
  const Decoration d = Decorate(entry);

  std::string label = entry.path;
  if (entry.change == Change::Renamed && !entry.old_path.empty()) {
    label = entry.old_path + " → " + entry.path;
  }

  Element row = hbox({
      text(selected ? " ▍" : "  ") | color(theme().accent),
      text(d.glyph) | color(d.color),
      text(" "),
      text(d.letter) | bold | color(d.color),
      text("  "),
      text(label) | color(selected ? theme().text : theme().text_dim),
      filler(),
  });

  if (selected) {
    row = row | bgcolor(theme().surface_alt) | focus;
  }
  return row;
}

}  // namespace

Element KeyCap(const std::string& key) {
  return text(" " + key + " ") | bold | color(theme().accent);
}

Decorator PaneFrame() {
  return borderRounded | color(theme().border_focus) | bgcolor(theme().surface);
}

Element Header(const model::StatusSnapshot& snapshot) {
  std::string branch = snapshot.branch;
  if (snapshot.head_unborn) {
    branch += " · unborn";
  } else if (snapshot.head_detached) {
    branch += " · detached";
  }

  return hbox({
             text(" gittop ") | bold | color(theme().bg) | bgcolor(theme().accent),
             text("  " + snapshot.repo_name) | bold | color(theme().text),
             filler(),
             text("◆ ") | color(theme().staged),
             text(branch + " ") | color(theme().text_dim),
         }) |
         bgcolor(theme().surface);
}

Element SummaryRow(const model::StatusSnapshot& snapshot) {
  const std::size_t total = snapshot.total();
  return hbox({
             Card("STAGED", snapshot.staged, total, theme().staged),
             separator() | color(theme().border),
             Card("UNSTAGED", snapshot.unstaged, total, theme().unstaged),
             separator() | color(theme().border),
             Card("UNTRACKED", snapshot.untracked, total, theme().untracked),
             separator() | color(theme().border),
             Card("CONFLICTS", snapshot.conflicted, total, theme().conflict),
         }) |
         borderRounded | color(theme().border);
}

Element FileList(const model::StatusSnapshot& snapshot, int selected) {
  Elements rows;

  if (snapshot.clean()) {
    rows.push_back(filler());
    rows.push_back(hbox({
        filler(),
        text("nothing to commit, working tree clean") | color(theme().text_faint),
        filler(),
    }));
    rows.push_back(filler());
  } else {
    rows.reserve(snapshot.entries.size());
    for (std::size_t i = 0; i < snapshot.entries.size(); ++i) {
      rows.push_back(Row(snapshot.entries[i], static_cast<int>(i) == selected));
    }
  }

  Element title = hbox({
      text(" CHANGES ") | bold | color(theme().text),
      text(std::to_string(snapshot.total()) + " ") | color(theme().text_faint),
  });

  return window(title, vbox(std::move(rows)) | yframe) | color(theme().border);
}

Element Footer(const std::string& message, bool is_error) {
  Elements lines;

  if (!message.empty()) {
    lines.push_back(hbox({
                        text(is_error ? " ✗ " : " ✓ "),
                        text(message),
                        filler(),
                    }) |
                    color(is_error ? theme().danger : theme().success));
  }

  lines.push_back(hbox({
                      KeyCap("space"), Hint("stage/unstage"),
                      KeyCap("a"), Hint("all"),
                      KeyCap("d"), Hint("discard"),
                      KeyCap("c"), Hint("commit"),
                      KeyCap("r"), Hint("refresh"),
                      filler(),
                      KeyCap("?"), Hint("help"),
                      KeyCap("q"), Hint("quit"),
                  }) |
                  bgcolor(theme().surface));

  return vbox(std::move(lines));
}

Element HelpPane() {
  auto line = [](const std::string& keys, const std::string& what) {
    return hbox({
        text("  "),
        text(keys) | bold | color(theme().accent) | size(WIDTH, EQUAL, 14),
        text(what) | color(theme().text_dim),
    });
  };

  return vbox({
             text(" Keys") | bold | color(theme().text),
             separator() | color(theme().border),
             line("j / ↓", "move down"),
             line("k / ↑", "move up"),
             line("g / G", "jump to first / last"),
             line("space", "stage or unstage the selection"),
             line("s / u", "stage / unstage explicitly"),
             line("a", "stage every change"),
             line("d", "discard the selection, after a confirm"),
             line("c", "write a commit"),
             line("r", "re-read the repository"),
             line("? ", "close this help"),
             line("q", "quit"),
             separator() | color(theme().border),
             hbox({
                 text("  filled ● staged   hollow ○ unstaged   ! conflict") |
                     color(theme().text_faint),
             }),
         }) |
         PaneFrame() | size(WIDTH, GREATER_THAN, 56);
}

Element ConfirmPane(const std::string& question, const std::string& detail) {
  return vbox({
             hbox({text(" ⚠ ") | color(theme().danger),
                   text(question) | bold | color(theme().text)}),
             separator() | color(theme().border),
             text("  " + detail) | color(theme().text_dim),
             text("  This cannot be undone.") | color(theme().danger),
             separator() | color(theme().border),
             hbox({
                 KeyCap("y"),
                 Hint("discard"),
                 KeyCap("n / esc"),
                 Hint("keep"),
             }),
         }) |
         PaneFrame() | size(WIDTH, GREATER_THAN, 52);
}

}  // namespace gittop::ui
