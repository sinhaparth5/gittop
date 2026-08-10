#include "ui/panels.hpp"

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "ui/theme.hpp"
#include "ui/widgets.hpp"

namespace gittop::ui {
namespace {

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
  Swatch color;
};

// Filled marker means staged, hollow means not. The letter carries the change
// type, so a row still reads correctly to someone who cannot separate green
// from amber: shape and letter say it before color does.
Decoration Decorate(const StatusEntry& e) {
  const Theme& t = theme();
  switch (e.stage) {
    case Stage::Conflict:
      return {"◆", "U", t.conflict};
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

enum class Group { Conflicts, Staged, Unstaged, Untracked };

Group GroupOf(const StatusEntry& e) {
  if (e.stage == Stage::Conflict) {
    return Group::Conflicts;
  }
  if (e.stage == Stage::Index) {
    return Group::Staged;
  }
  return e.change == Change::Untracked ? Group::Untracked : Group::Unstaged;
}

struct GroupStyle {
  std::string label;
  Swatch color;
};

GroupStyle StyleOf(Group g) {
  const Theme& t = theme();
  switch (g) {
    case Group::Conflicts:
      return {"CONFLICTS", t.conflict};
    case Group::Staged:
      return {"STAGED", t.staged};
    case Group::Unstaged:
      return {"UNSTAGED", t.unstaged};
    case Group::Untracked:
      return {"UNTRACKED", t.untracked};
  }
  return {"", t.text_dim};
}

std::size_t CountOf(Group g, const model::StatusSnapshot& s) {
  switch (g) {
    case Group::Conflicts:
      return s.conflicted;
    case Group::Staged:
      return s.staged;
    case Group::Unstaged:
      return s.unstaged;
    case Group::Untracked:
      return s.untracked;
  }
  return 0;
}

Element GroupHeader(Group g, std::size_t count) {
  const GroupStyle style = StyleOf(g);
  return hbox({
      text("  ▍") | color(style.color),
      text(" " + style.label + "  ") | bold | color(style.color),
      text(std::to_string(count)) | color(theme().text_faint),
      filler(),
  });
}

Element Row(const StatusEntry& entry, bool selected) {
  const Theme& t = theme();
  const Decoration d = Decorate(entry);

  Element path = PathText(entry.path, selected);
  if (entry.change == Change::Renamed && !entry.old_path.empty()) {
    path = hbox({
        PathText(entry.old_path, false),
        text("  →  ") | color(t.text_faint),
        PathText(entry.path, selected),
    });
  }

  Element row = hbox({
      text(selected ? " ▌" : "  ") | color(t.accent),
      text("  "),
      text(d.glyph) | color(d.color),
      text("  "),
      text(d.letter) | bold | color(d.color),
      text("   "),
      std::move(path),
      filler(),
  });

  if (selected) {
    row = row | bgcolor(t.surface_alt) | focus;
  }
  return row;
}

Element StatCard(const std::string& label, std::size_t value, float bar, Ramp ramp,
                 Swatch accent) {
  const Theme& t = theme();
  // Fixing the label width equalises every card's minimum, so xflex divides the
  // row evenly. Without it the cards inherit the length of their own label and
  // the four bars end up different lengths for identical values.
  constexpr int kLabelWidth = 10;
  return vbox({
             hbox({
                 text(label) | color(t.text_faint) | size(WIDTH, EQUAL, kLabelWidth),
                 filler(),
                 text(std::to_string(value)) | bold | color(value > 0 ? accent : t.text_faint),
             }),
             text(""),
             GradientBar(bar, ramp, t.surface_alt),
         }) |
         xflex;
}

Element EmptyState() {
  const Theme& t = theme();
  return vbox({
      filler(),
      hbox({filler(), text("✓") | bold | color(t.success), filler()}),
      text(""),
      hbox({filler(), text("working tree clean") | color(t.text), filler()}),
      hbox({filler(), text("nothing to stage or commit") | color(t.text_faint), filler()}),
      filler(),
  });
}

}  // namespace

Decorator PaneFrame() {
  return borderRounded | color(theme().border_focus) | bgcolor(theme().surface);
}

Element Header(const model::StatusSnapshot& snapshot) {
  const Theme& t = theme();
  const std::size_t total = snapshot.total();

  Elements parts{
      text(" gittop ") | bold | color(t.bg) | bgcolor(t.accent),
      text("  "),
      text(snapshot.repo_name) | bold | color(t.text),
      text("   "),
      text("◆") | color(t.staged),
      text(" " + snapshot.branch) | color(t.text_dim),
      filler(),
  };

  if (snapshot.head_unborn) {
    parts.push_back(text(" unborn ") | bold | color(t.bg) | bgcolor(t.warning));
    parts.push_back(text("  "));
  } else if (snapshot.head_detached) {
    parts.push_back(text(" detached ") | bold | color(t.bg) | bgcolor(t.warning));
    parts.push_back(text("  "));
  }

  parts.push_back(text(std::to_string(total)) | bold |
                  color(total > 0 ? t.text : t.text_faint));
  parts.push_back(text(total == 1 ? " change " : " changes ") | color(t.text_faint));

  return hbox(std::move(parts)) | bgcolor(t.surface);
}

Element TabBar(View active) {
  const Theme& t = theme();

  const auto tab = [&t, active](View view, const std::string& key, const std::string& label) {
    const bool on = view == active;
    return hbox({
        text(" " + key + " ") | bold | color(on ? t.bg : t.text_faint) |
            bgcolor(on ? t.accent : t.surface),
        text(" " + label + "  ") | bold | color(on ? t.text : t.text_faint) |
            bgcolor(on ? t.surface_alt : t.surface),
    });
  };

  return hbox({
             text(" "),
             tab(View::Status, "1", "Status"),
             text(" "),
             tab(View::History, "2", "History"),
             text(" "),
             tab(View::Branches, "3", "Branches"),
             text(" "),
             tab(View::Graph, "4", "Graph"),
             text(" "),
             tab(View::Remote, "5", "Remote"),
             text(" "),
             tab(View::Pipelines, "6", "CI"),
             filler(),
         }) |
         bgcolor(t.surface);
}

Element SummaryRow(const model::StatusSnapshot& snapshot, const StatBars& bars,
                   bool compact) {
  const Theme& t = theme();
  const auto gap = [] { return text("  "); };
  const auto rule = [&t] { return separator() | color(t.border); };

  Element staged = StatCard("STAGED", snapshot.staged, bars.staged, t.staged_ramp, t.staged);
  Element unstaged =
      StatCard("UNSTAGED", snapshot.unstaged, bars.unstaged, t.unstaged_ramp, t.unstaged);
  Element untracked =
      StatCard("UNTRACKED", snapshot.untracked, bars.untracked, t.untracked_ramp, t.untracked);
  Element conflicts =
      StatCard("CONFLICTS", snapshot.conflicted, bars.conflicted, t.conflict_ramp, t.conflict);

  if (compact) {
    return vbox({
               hbox({gap(), std::move(staged), gap(), rule(), gap(), std::move(unstaged), gap()}),
               separator() | color(t.border),
               hbox({gap(), std::move(untracked), gap(), rule(), gap(), std::move(conflicts),
                     gap()}),
           }) |
           borderRounded | color(t.border) | bgcolor(t.surface);
  }

  return hbox({
             gap(),
             std::move(staged),
             gap(),
             rule(),
             gap(),
             std::move(unstaged),
             gap(),
             rule(),
             gap(),
             std::move(untracked),
             gap(),
             rule(),
             gap(),
             std::move(conflicts),
             gap(),
         }) |
         borderRounded | color(t.border) | bgcolor(t.surface);
}

Element FileList(const model::StatusSnapshot& snapshot, int selected) {
  const Theme& t = theme();
  Elements rows;

  if (snapshot.clean()) {
    rows.push_back(EmptyState());
  } else {
    std::optional<Group> current;
    for (std::size_t i = 0; i < snapshot.entries.size(); ++i) {
      const StatusEntry& entry = snapshot.entries[i];
      const Group group = GroupOf(entry);

      if (!current.has_value() || *current != group) {
        if (current.has_value()) {
          rows.push_back(text(""));  // breathing room between groups
        }
        rows.push_back(GroupHeader(group, CountOf(group, snapshot)));
        current = group;
      }
      rows.push_back(Row(entry, static_cast<int>(i) == selected));
    }
    rows.push_back(text(""));
  }

  Element title = text(" CHANGES ") | bold | color(t.text_dim);

  return window(title, vbox(std::move(rows)) | vscroll_indicator | yframe) | color(t.border) |
         bgcolor(t.surface);
}

Element Footer(const std::string& message, bool is_error, float fade, View view) {
  const Theme& t = theme();

  // The toast line is always drawn, blank or not, so the list above never
  // resizes under the cursor when a message arrives or ages out.
  Element toast = text(" ") | bgcolor(t.bg);
  if (!message.empty() && fade > 0.01F) {
    const Rgb accent_rgb = is_error ? t.danger.rgb : t.success.rgb;
    toast = hbox({
                text("  "),
                text(is_error ? "✗" : "✓") | bold | color(ToColor(Mix(t.bg.rgb, accent_rgb, fade))),
                text("  " + message) | color(ToColor(Mix(t.bg.rgb, t.text.rgb, fade))),
                filler(),
            }) |
            bgcolor(t.bg);
  }

  // Six chips fit an 80-column terminal with room to spare. Everything else
  // lives in the help overlay rather than being clipped in half here, which is
  // what a seventh chip was doing.
  Elements chips{text(" ")};
  if (view == View::Status) {
    chips.push_back(Chip("space", "stage"));
    chips.push_back(Chip("a", "all"));
    chips.push_back(Chip("d", "discard"));
    chips.push_back(Chip("c", "commit"));
  } else if (view == View::Graph) {
    chips.push_back(Chip("h/l", "pan"));
    chips.push_back(Chip("d/w/m", "bucket"));
    chips.push_back(Chip("tab", "switch view"));
  } else if (view == View::Remote) {
    chips.push_back(Chip("r", "fetch"));
    chips.push_back(Chip("tab", "switch view"));
  } else if (view == View::Pipelines) {
    chips.push_back(Chip("j/k", "move"));
    chips.push_back(Chip("enter", "jobs"));
    chips.push_back(Chip("r", "refresh"));
  } else {
    chips.push_back(Chip("j/k", "move"));
    chips.push_back(Chip("tab", "switch view"));
    chips.push_back(Chip("r", "reload"));
  }
  chips.push_back(filler());
  chips.push_back(Chip("?", "help"));
  chips.push_back(Chip("q", "quit"));

  Element keys = hbox(std::move(chips)) | bgcolor(t.surface);

  return vbox({toast, keys});
}

Element HelpPane() {
  const Theme& t = theme();
  const auto line = [&t](const std::string& keys, const std::string& what) {
    return hbox({
        text("  "),
        text(keys) | bold | color(t.accent) | size(WIDTH, EQUAL, 12),
        text(what) | color(t.text_dim),
    });
  };

  return vbox({
             hbox({text(" Keys") | bold | color(t.text), filler()}),
             separator() | color(t.border),
             line("1 … 6", "status / history / branches / graph / remote / CI"),
             line("tab", "cycle through the views"),
             line("j / ↓", "move down"),
             line("k / ↑", "move up"),
             line("g / G", "first / last, or oldest / newest on the graph"),
             line("h / l", "pan the graph through time"),
             line("d / w / m", "graph bucket: day, week, month"),
             line("space", "stage or unstage the selection"),
             line("s / u", "stage / unstage explicitly"),
             line("a", "stage everything"),
             line("d", "discard the selection, after a confirm"),
             line("c", "write a commit"),
             line("enter", "jobs of the selected CI run"),
             line("r", "re-read the repository, or re-fetch the remote"),
             line("q", "quit"),
             separator() | color(t.border),
             hbox({
                 text("  "),
                 text("●") | color(t.staged),
                 text(" staged   ") | color(t.text_faint),
                 text("○") | color(t.unstaged),
                 text(" unstaged   ") | color(t.text_faint),
                 text("○") | color(t.untracked),
                 text(" untracked   ") | color(t.text_faint),
                 text("◆") | color(t.conflict),
                 text(" conflict") | color(t.text_faint),
             }),
             separator() | color(t.border),
             hbox({filler(), Chip("any key", "close"), filler()}),
         }) |
         PaneFrame() | size(WIDTH, GREATER_THAN, 62);
}

Element ConfirmPane(const std::string& question, const std::string& detail) {
  const Theme& t = theme();
  return vbox({
             hbox({
                 text(" ▲ ") | bold | color(t.danger),
                 text(question) | bold | color(t.text),
                 filler(),
             }),
             separator() | color(t.border),
             text(""),
             hbox({text("   "), PathText(detail, true)}),
             text(""),
             hbox({text("   "), text("This cannot be undone.") | color(t.danger)}),
             text(""),
             separator() | color(t.border),
             hbox({
                 text(" "),
                 Chip("y", "discard"),
                 filler(),
                 Chip("n / esc", "keep"),
             }),
         }) |
         borderRounded | color(t.danger) | bgcolor(t.surface) | size(WIDTH, GREATER_THAN, 54);
}

}  // namespace gittop::ui
