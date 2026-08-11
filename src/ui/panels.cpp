#include "ui/panels.hpp"

#include <algorithm>
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

std::string TabLabel(View view) {
  switch (view) {
    case View::Status:
      return "Status";
    case View::History:
      return "History";
    case View::Branches:
      return "Branches";
    case View::Graph:
      return "Graph";
    case View::Diff:
      return "Diff";
    case View::Stashes:
      return "Stashes";
    case View::Remote:
      return "Remote";
    case View::Pipelines:
      return "CI";
    case View::Pulls:
      return "Pulls";
  }
  return "?";
}

// The name a config writes and the view it means. One table for both
// directions, so a view can never be parseable under a name it does not print.
const struct {
  const char* name;
  View view;
} kViewNames[] = {
    {"status", View::Status},   {"history", View::History},     {"branches", View::Branches},
    {"graph", View::Graph},     {"diff", View::Diff},           {"stashes", View::Stashes},
    {"remote", View::Remote},   {"ci", View::Pipelines},        {"pulls", View::Pulls},
};

// A chip whose key comes from the live binding. Empty when the action has been
// unbound in the config, in which case there is nothing to advertise.
Element KeyChip(const Keymap& keys, Action action, const std::string& label) {
  const std::string key = keys.KeyFor(action);
  if (key.empty()) {
    return text("");
  }
  return Chip(key, label);
}

// "j / ↓", or just the first when only one key is bound. Arrow names are
// printed as arrows: a hint that reads "down" next to "j" says less.
std::string KeyList(const Keymap& keys, Action action) {
  static const struct {
    const char* name;
    const char* glyph;
  } kGlyphs[] = {{"up", "↑"}, {"down", "↓"}, {"left", "←"}, {"right", "→"},
                 {"enter", "⏎"}};

  std::string out;
  for (const std::string& key : keys.KeysFor(action)) {
    std::string shown = key;
    for (const auto& glyph : kGlyphs) {
      if (key == glyph.name) {
        shown = glyph.glyph;
        break;
      }
    }
    out += out.empty() ? shown : " / " + shown;
  }
  return out;
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

namespace {

// Mutable for the same reason the active theme is: the config replaces it once,
// on the UI thread, before the first frame, and every reader afterwards is on
// that same thread during Render.
std::vector<View> g_views{
    View::Status,  View::History,   View::Branches, View::Graph, View::Diff,
    View::Stashes, View::Remote,    View::Pipelines, View::Pulls,
};

}  // namespace

const std::vector<View>& AllViews() {
  return g_views;
}

bool SetViews(const std::vector<View>& views) {
  if (views.empty()) {
    return false;
  }
  g_views = views;
  return true;
}

bool ParseViewName(const std::string& name, View* out) {
  for (const auto& entry : kViewNames) {
    if (name == entry.name) {
      *out = entry.view;
      return true;
    }
  }
  return false;
}

std::string ViewName(View view) {
  for (const auto& entry : kViewNames) {
    if (entry.view == view) {
      return entry.name;
    }
  }
  return "?";
}

Element TabBar(View active, std::vector<Box>* tabs) {
  const Theme& t = theme();
  const std::vector<View>& views = AllViews();

  if (tabs != nullptr) {
    tabs->assign(views.size(), Box());
  }

  Elements row{text(" ")};
  for (std::size_t i = 0; i < views.size(); ++i) {
    const View view = views[i];
    const bool on = view == active;
    // The digit shown is the key that reaches the tab, so it comes from the
    // view's own position rather than from a literal that could drift.
    const std::string key = std::to_string(i + 1);

    Element chip = hbox({
        text(" " + key + " ") | bold | color(on ? t.bg : t.text_faint) |
            bgcolor(on ? t.accent : t.surface),
        text(" " + TabLabel(view) + "  ") | bold | color(on ? t.text : t.text_faint) |
            bgcolor(on ? t.surface_alt : t.surface),
    });
    if (tabs != nullptr) {
      chip = std::move(chip) | reflect((*tabs)[i]);
    }
    row.push_back(std::move(chip));
    row.push_back(text(" "));
  }
  row.push_back(filler());

  return hbox(std::move(row)) | bgcolor(t.surface);
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

Element FileList(const model::StatusSnapshot& snapshot, int selected,
                 std::vector<Box>* row_boxes) {
  const Theme& t = theme();
  Elements rows;

  if (row_boxes != nullptr) {
    row_boxes->assign(snapshot.entries.size(), Box());
  }

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
      Element row = Row(entry, static_cast<int>(i) == selected);
      if (row_boxes != nullptr) {
        // The group headers and blank spacers mean a row's screen position has
        // nothing to do with its index, which is exactly why this is reflected
        // rather than computed.
        row = std::move(row) | reflect((*row_boxes)[i]);
      }
      rows.push_back(std::move(row));
    }
    rows.push_back(text(""));
  }

  Element title = text(" CHANGES ") | bold | color(t.text_dim);

  return window(title, vbox(std::move(rows)) | vscroll_indicator | yframe) | color(t.border) |
         bgcolor(t.surface);
}

Element Footer(const std::string& message, bool is_error, float fade, View view,
               const Keymap& keys, const std::string& filter) {
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
  const std::string move = KeyList(keys, Action::Down) + " " + KeyList(keys, Action::Up);

  Elements chips{text(" ")};
  if (view == View::Status) {
    chips.push_back(KeyChip(keys, Action::ToggleStage, "stage"));
    chips.push_back(KeyChip(keys, Action::StageAll, "all"));
    chips.push_back(KeyChip(keys, Action::Discard, "discard"));
    chips.push_back(KeyChip(keys, Action::Commit, "commit"));
    chips.push_back(KeyChip(keys, Action::Push, "push"));
  } else if (view == View::Graph) {
    chips.push_back(Chip(keys.KeyFor(Action::PanLeft) + "/" + keys.KeyFor(Action::PanRight),
                         "pan"));
    chips.push_back(Chip(keys.KeyFor(Action::BucketDay) + "/" + keys.KeyFor(Action::BucketWeek) +
                             "/" + keys.KeyFor(Action::BucketMonth),
                         "bucket"));
    chips.push_back(KeyChip(keys, Action::NextView, "switch view"));
  } else if (view == View::Remote) {
    chips.push_back(KeyChip(keys, Action::Reload, "fetch"));
    chips.push_back(KeyChip(keys, Action::NextRemote, "remote"));
    chips.push_back(KeyChip(keys, Action::Pull, "pull"));
    chips.push_back(KeyChip(keys, Action::Push, "push"));
  } else if (view == View::Pipelines) {
    chips.push_back(Chip(move, "move"));
    chips.push_back(KeyChip(keys, Action::Open, "jobs"));
    chips.push_back(KeyChip(keys, Action::Reload, "refresh"));
  } else if (view == View::Pulls) {
    chips.push_back(Chip(move, "move"));
    chips.push_back(KeyChip(keys, Action::Open, "details"));
    chips.push_back(KeyChip(keys, Action::Reload, "refresh"));
  } else if (view == View::Diff) {
    chips.push_back(Chip(move, "scroll"));
    chips.push_back(Chip(keys.KeyFor(Action::DiffPrevFile) + "/" +
                             keys.KeyFor(Action::DiffNextFile),
                         "file"));
    chips.push_back(KeyChip(keys, Action::DiffSwitch, "staged"));
  } else if (view == View::Stashes) {
    chips.push_back(KeyChip(keys, Action::StashSave, "stash"));
    chips.push_back(KeyChip(keys, Action::StashApply, "apply"));
    chips.push_back(KeyChip(keys, Action::StashPop, "pop"));
    chips.push_back(KeyChip(keys, Action::StashDrop, "drop"));
  } else {
    chips.push_back(Chip(move, "move"));
    chips.push_back(KeyChip(keys, Action::NextView, "switch view"));
    chips.push_back(KeyChip(keys, Action::Reload, "reload"));
  }
  chips.push_back(filler());

  // An active filter goes where the eye already is when a list looks short. A
  // list hiding nine rows out of ten and a list with one row in it are
  // indistinguishable otherwise, and only one of them is a problem.
  if (!filter.empty()) {
    chips.push_back(text(" / ") | bold | color(t.bg) | bgcolor(t.warning));
    chips.push_back(text(" " + filter + " ") | color(t.text) | bgcolor(t.surface_raised));
    chips.push_back(text("  "));
  }
  chips.push_back(KeyChip(keys, Action::Help, "help"));
  chips.push_back(KeyChip(keys, Action::Quit, "quit"));

  Element key_row = hbox(std::move(chips)) | bgcolor(t.surface);

  return vbox({toast, key_row});
}

Element OperationBanner(const model::OperationState& state, const Keymap& keys) {
  if (!state.active()) {
    // Not an empty line: an empty line still costs a row, and the status view
    // has better uses for it than reserving space against a merge that is not
    // happening. Nothing here reflows under the cursor, so it can come and go.
    return text("");
  }
  const Theme& t = theme();

  Elements parts{
      text(" ⚠ ") | bold | color(t.bg) | bgcolor(t.warning),
      text("  "),
      text(std::string(model::OperationName(state.operation)) + " in progress") | bold |
          color(t.text),
  };
  if (state.total > 0) {
    parts.push_back(text("  " + std::to_string(state.step) + " of " +
                         std::to_string(state.total)) |
                    color(t.text_dim));
  }
  if (!state.detail.empty()) {
    parts.push_back(text("  " + state.detail) | color(t.text_faint));
  }
  parts.push_back(filler());
  parts.push_back(KeyChip(keys, Action::Operation, "continue or abort"));
  parts.push_back(text(" "));

  return hbox(std::move(parts)) | bgcolor(t.surface_raised);
}

Element OperationPane(const model::OperationState& state) {
  const Theme& t = theme();
  const std::string name = model::OperationName(state.operation);
  const bool continuable = state.operation == model::Operation::Rebase;

  Elements rows{
      hbox({
          text(" ▲ ") | bold | color(t.warning),
          text(name.empty() ? "Nothing in progress" : name + " in progress") | bold |
              color(t.text),
          filler(),
      }),
      separator() | color(t.border),
      text(""),
  };

  if (!state.active()) {
    rows.push_back(hbox({text("   "),
                         text("This repository is not in the middle of anything.") |
                             color(t.text_dim)}));
  } else {
    if (state.total > 0) {
      rows.push_back(hbox({text("   "),
                           text("step " + std::to_string(state.step) + " of " +
                                std::to_string(state.total)) |
                               color(t.text)}));
    }
    if (!state.detail.empty()) {
      rows.push_back(hbox({text("   "), text(state.detail) | color(t.text_dim)}));
    }
    rows.push_back(text(""));
    // The two answers do different amounts of damage and the pane says which
    // is which, because "abort" reads as "stop bothering me" until you know it
    // also means "throw away what is in the tree".
    if (continuable) {
      rows.push_back(hbox({text("   "), text("continue") | bold | color(t.success),
                           text("  commits what is staged and carries on") | color(t.text_dim)}));
    } else {
      // Only a rebase has a "continue": a merge or a cherry-pick is finished by
      // writing an ordinary commit, and offering a key that would answer "no
      // rebase in progress" is worse than not offering one.
      rows.push_back(hbox({text("   "),
                           text("Stage the resolved files and commit to finish it.") |
                               color(t.text_dim)}));
    }
    rows.push_back(hbox({text("   "), text("abort   ") | bold | color(t.danger),
                         text("  puts the branch back and discards the attempt") |
                             color(t.text_dim)}));
  }

  rows.push_back(text(""));
  rows.push_back(separator() | color(t.border));
  rows.push_back(hbox({
      text(" "),
      continuable ? Chip("c", "continue") : text(""),
      text(" "),
      state.active() ? Chip("a", "abort") : text(""),
      filler(),
      Chip("esc", "close"),
  }));

  return vbox(std::move(rows)) | PaneFrame() | size(WIDTH, GREATER_THAN, 62);
}

Element FilterPane(Element input, const std::string& scope, int matches, int total) {
  const Theme& t = theme();

  // The count is the whole point of showing this while typing: it turns a
  // filter that matches nothing from a blank list into a number that says so
  // before you have finished the word.
  const bool nothing = total > 0 && matches == 0;

  return vbox({
             hbox({
                 text(" Filter ") | bold | color(t.accent),
                 text(scope) | color(t.text_faint),
                 filler(),
                 text(std::to_string(matches) + " of " + std::to_string(total) + " ") |
                     color(nothing ? t.danger : t.text_dim),
             }),
             separator() | color(t.border),
             hbox({
                 text("  / ") | bold | color(t.warning),
                 std::move(input) | flex,
             }),
             separator() | color(t.border),
             hbox({
                 text(" "),
                 Chip("enter", "keep it"),
                 filler(),
                 Chip("esc", "clear"),
             }),
         }) |
         PaneFrame() | size(WIDTH, GREATER_THAN, 56);
}

Element HelpPane(const Keymap& keys, int width, int height, int scroll) {
  const Theme& t = theme();
  // Sixteen, not twelve: "ctrl-u / ctrl-d" is fifteen cells and was being cut
  // in half by the description next to it.
  constexpr int kKeyColumn = 16;
  const auto row = [&t](const std::string& shown, const std::string& what) {
    return hbox({
        text("  "),
        text(shown) | bold | color(t.accent) | size(WIDTH, EQUAL, kKeyColumn),
        text(what) | color(t.text_dim),
    });
  };
  // Every key here is read out of the live keymap, so a config that rebinds one
  // is described correctly rather than contradicted by its own help screen.
  const auto line = [&row, &keys](Action action, const std::string& what) {
    return row(KeyList(keys, action), what);
  };
  const auto pair = [&row, &keys](Action a, Action b, const std::string& what) {
    return row(keys.KeyFor(a) + " / " + keys.KeyFor(b), what);
  };
  const auto heading = [&t](const std::string& what) {
    return hbox({text("  "), text(what) | bold | color(t.text_faint)});
  };

  Elements left{
      heading("MOVING"),
      // Slots rather than names, because [layout] views decides which view each
      // digit reaches and the tab bar prints the same number.
      row(keys.KeyFor(Action::View1) + " … " + keys.KeyFor(Action::View9),
          "jump to a tab by its number"),
      line(Action::NextView, "cycle through the views"),
      line(Action::Down, "move down"),
      line(Action::Up, "move up"),
      pair(Action::First, Action::Last, "first / last, or the ends of the graph"),
      pair(Action::PageUp, Action::PageDown, "move a screen at a time"),
      line(Action::Filter, "filter this list; esc clears it"),
      line(Action::Open, "diff a file or commit, jobs, pull details"),
      text(""),
      heading("THE GRAPH"),
      pair(Action::PanLeft, Action::PanRight, "pan through time"),
      row(keys.KeyFor(Action::BucketDay) + " / " + keys.KeyFor(Action::BucketWeek) + " / " +
              keys.KeyFor(Action::BucketMonth),
          "bucket: day, week, month"),
      text(""),
      heading("CHANGES"),
      line(Action::ToggleStage, "stage or unstage the selection"),
      pair(Action::Stage, Action::Unstage, "stage / unstage explicitly"),
      line(Action::StageAll, "stage everything"),
      line(Action::Discard, "discard the selection, after a confirm"),
      line(Action::Commit, "write a commit"),
  };

  Elements right{
      heading("THE DIFF"),
      line(Action::DiffSwitch, "swap between unstaged and staged"),
      pair(Action::DiffPrevFile, Action::DiffNextFile, "previous / next file"),
      text(""),
      heading("STASHES"),
      line(Action::StashSave, "stash the whole working tree"),
      line(Action::StashApply, "apply, keeping the entry"),
      line(Action::StashPop, "apply and drop it, after a confirm"),
      line(Action::StashDrop, "drop it, after a confirm"),
      text(""),
      heading("HISTORY SURGERY"),
      line(Action::Rebase, "rebase onto the upstream, after a confirm"),
      line(Action::Operation, "continue or abort what is in progress"),
      text(""),
      heading("THE REMOTE"),
      line(Action::Fetch, "fetch from the active remote"),
      line(Action::Pull, "pull, fast-forward only"),
      line(Action::Push, "push this branch, after a confirm"),
      line(Action::NextRemote, "switch to the next remote"),
      text(""),
      heading("EVERYWHERE"),
      line(Action::Theme, "next theme"),
      line(Action::Reload, "re-read the repository or the remote"),
      line(Action::Quit, "quit"),
      row("mouse", "wheel scrolls, click selects a row or tab"),
      // What the screen is actually being drawn with. The first question about
      // a terminal that looks wrong is which of these two it is.
      row("theme", ThemeLabel() + " · " + ColorDepthName(ColorDepthNow())),
  };

  Elements legend{
      text("  "),
      text("●") | color(t.staged),
      text(" staged   ") | color(t.text_faint),
      text("○") | color(t.unstaged),
      text(" unstaged   ") | color(t.text_faint),
      text("○") | color(t.untracked),
      text(" untracked   ") | color(t.text_faint),
      text("◆") | color(t.conflict),
      text(" conflict") | color(t.text_faint),
  };

  // Two columns wherever both fit. One column is forty-odd rows and gets its
  // bottom half clipped off a standard terminal with nothing saying so, which
  // is a help screen that hides half the help.
  constexpr int kColumnWidth = 58;
  constexpr int kChrome = 6;  // title, two separators, the legend, the chip
  const bool side_by_side =
      width >= (2 * kColumnWidth) + 4 &&
      height < static_cast<int>(left.size() + right.size()) + kChrome;

  Element body;
  if (side_by_side) {
    body = hbox({
        vbox(std::move(left)) | size(WIDTH, EQUAL, kColumnWidth),
        separator() | color(t.border),
        vbox(std::move(right)) | size(WIDTH, EQUAL, kColumnWidth),
    });
  } else {
    Elements all = std::move(left);
    all.push_back(text(""));
    for (Element& element : right) {
      all.push_back(std::move(element));
    }
    // One column on a terminal too short for it is the case scrolling exists
    // for: forty rows in twenty is half a help screen, and a reader has no way
    // to know the other half is there. The caller keeps the offset and routes
    // the movement keys here instead of closing on them.
    const auto index = static_cast<std::size_t>(
        std::clamp(scroll, 0, std::max(0, static_cast<int>(all.size()) - 1)));
    all[index] = std::move(all[index]) | ftxui::focus;
    body = vbox(std::move(all)) | vscroll_indicator | yframe;
  }

  return vbox({
             hbox({text(" Keys") | bold | color(t.text), filler()}),
             separator() | color(t.border),
             std::move(body),
             separator() | color(t.border),
             hbox(std::move(legend)),
             separator() | color(t.border),
             hbox({filler(),
                   side_by_side ? Chip("any key", "close")
                                : hbox({Chip("j / k", "scroll"), text("  "),
                                        Chip("any other key", "close")}),
                   filler()}),
         }) |
         PaneFrame() | size(WIDTH, GREATER_THAN, 74);
}

Element ConfirmPane(const std::string& question, const std::string& detail,
                    const std::string& warning, const std::string& confirm_label) {
  const Theme& t = theme();
  // A question that carries no warning is not a destructive one, and colouring
  // its frame red anyway would spend the alarm on something that does not
  // deserve it — leaving nothing left for the one that does.
  const Swatch tone = warning.empty() ? t.accent : t.danger;

  Elements rows{
      hbox({
          text(" ▲ ") | bold | color(tone),
          text(question) | bold | color(t.text),
          filler(),
      }),
      separator() | color(t.border),
      text(""),
      hbox({text("   "), PathText(detail, true)}),
      text(""),
  };
  if (!warning.empty()) {
    rows.push_back(hbox({text("   "), text(warning) | color(t.danger)}));
    rows.push_back(text(""));
  }
  rows.push_back(separator() | color(t.border));
  rows.push_back(hbox({
      text(" "),
      Chip("y", confirm_label),
      filler(),
      Chip("n / esc", "cancel"),
  }));

  return vbox(std::move(rows)) | borderRounded | color(tone) | bgcolor(t.surface) |
         size(WIDTH, GREATER_THAN, 54);
}

Element PassphrasePane(Element input, bool rejected) {
  const Theme& t = theme();

  Elements rows{
      text(" ssh key passphrase") | bold | color(t.accent),
      separator() | color(t.border),
      // A box asking for a passphrase is indistinguishable from the thing you
      // are told never to type into, so it says what it wants and where it goes.
      // Anything vaguer would be teaching a habit worth not teaching.
      hbox({text("  "),
            text("Your ssh key is encrypted and no agent is holding it.") | color(t.text_dim)}),
      hbox({text("  "),
            text("This goes to ssh for this transfer only, and is never saved.") |
                color(t.text_dim)}),
      text(""),
      hbox({
          text("  ❯ ") | color(t.accent),
          std::move(input) | flex,
      }),
  };

  if (rejected) {
    rows.push_back(text(""));
    rows.push_back(
        hbox({text("  "), text("ssh could not use the last one.") | color(t.danger)}));
  }

  rows.push_back(separator() | color(t.border));
  rows.push_back(hbox({
      text(" "),
      Chip("enter", "unlock"),
      filler(),
      Chip("esc", "cancel"),
  }));

  return vbox(std::move(rows)) | PaneFrame() | size(WIDTH, GREATER_THAN, 62);
}

Element TransferPane(const TransferView& view, int frame) {
  const Theme& t = theme();

  Elements rows{
      hbox({
          text(" "),
          text(SpinnerFrame(frame)) | bold | color(t.accent),
          text(" " + view.title) | bold | color(t.text),
          filler(),
      }),
      separator() | color(t.border),
      text(""),
      hbox({text("  "), text(view.phase) | color(t.text_dim), filler()}),
      text(""),
  };

  if (view.ratio >= 0.0F) {
    rows.push_back(hbox({text("  "), GradientBar(view.ratio, t.untracked_ramp, t.surface_alt),
                         text("  ")}));
    rows.push_back(text(""));
    std::string counted = std::to_string(view.objects) + " / " + std::to_string(view.total) +
                          " objects";
    if (view.bytes > 0) {
      counted += "   " + std::to_string(view.bytes / 1024) + " KiB";
    }
    rows.push_back(hbox({text("  "), text(counted) | color(t.text_faint), filler()}));
  } else {
    // No totals yet. A bar that guesses at a percentage during the negotiation
    // phase is worse than one that says it is still counting.
    rows.push_back(hbox({text("  "), text("counting…") | color(t.text_faint), filler()}));
  }

  if (!view.detail.empty()) {
    rows.push_back(text(""));
    rows.push_back(hbox({text("  "), text(view.detail) | color(t.text_faint), filler()}));
  }

  rows.push_back(text(""));
  rows.push_back(separator() | color(t.border));
  rows.push_back(hbox({
      text(" "),
      view.cancellable ? Chip("esc", "cancel") : text(""),
      text(" "),
      view.cancellable ? Chip("q", "cancel and quit") : text(""),
      filler(),
  }));

  return vbox(std::move(rows)) | PaneFrame() | size(WIDTH, GREATER_THAN, 56);
}

}  // namespace gittop::ui
