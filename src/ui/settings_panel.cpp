#include "ui/settings_panel.hpp"

#include <string>
#include <utility>
#include <vector>

#include "ui/glyphs.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

namespace gittop::ui {
namespace {

using ftxui::Box;
using ftxui::Element;
using ftxui::Elements;
using ftxui::bold;
using ftxui::bgcolor;
using ftxui::color;
using ftxui::filler;
using ftxui::flex;
using ftxui::hbox;
using ftxui::reflect;
using ftxui::size;
using ftxui::text;
using ftxui::vbox;
using ftxui::EQUAL;
using ftxui::WIDTH;
using model::Provider;
using model::TokenSource;

// Wide enough for "panel border" with a gap after it. Fixed, so every value in
// the list starts in the same column — a settings page whose values step in and
// out is the one shape that makes eight rows unreadable.
constexpr int kLabelCells = 15;

// The read-only column. Holds a remote name and enough of a URL to recognise
// which host it is, which is the width this is actually chosen for.
constexpr int kSidecarCells = 38;

// Below this the sidecar's facts are worth less than the room they take, and
// the settings list gets the whole width. Measured against the widest row the
// list can produce rather than rounded to a number that looked about right.
constexpr int kSidecarFloor = kSidecarCells + 46;

std::string OnOff(bool on) {
  return on ? "on" : "off";
}

// ----------------------------------------------------------------- the groups

SettingsGroup AccountGroup(const SettingsView& view) {
  SettingsGroup group;
  group.title = "ACCOUNT";

  SettingsRow connection;
  connection.label = "connection";
  switch (view.token_source) {
    case TokenSource::Environment:
      connection.value = "authenticated";
      // The variable's name, never its contents. Same rule as the remote panel.
      connection.note = view.token_origin;
      // Short, because it is printed in the aside column and again as a toast.
      // The variable it names is already in the note beside it.
      connection.blocked = "the environment wins; unset it to sign in here";
      break;

    case TokenSource::ConfigFile:
      connection.value = "signed in";
      connection.note = "saved in the config file";
      connection.action = SettingsAction::SignOut;
      connection.verb = "sign out";
      break;

    case TokenSource::SignedIn:
      // Says outright that it is not on disk, because that is the one thing
      // about this state the user would otherwise discover on the next launch.
      connection.value = "signed in";
      connection.note = "this session only";
      connection.action = SettingsAction::SignOut;
      connection.verb = "sign out";
      break;

    case TokenSource::None:
      connection.value = "anonymous";
      if (view.host.empty()) {
        connection.note = "no GitHub or GitLab remote";
        connection.blocked = "needs a GitHub or GitLab remote";
      } else {
        connection.note = "public data only";
        connection.action = SettingsAction::SignIn;
        connection.verb = "connect";
      }
      break;
  }
  group.rows.push_back(std::move(connection));

  SettingsRow remote;
  remote.label = "remote";
  if (view.remotes.empty()) {
    remote.value = "none";
    remote.blocked = "this repository has no remote";
  } else {
    for (const SettingsRemote& entry : view.remotes) {
      if (entry.active) {
        remote.value = entry.name;
        break;
      }
    }
    remote.note = ProviderGlyph(view.provider) + " " +
                  (view.host.empty() ? model::ProviderName(view.provider) : view.host);
    if (view.remotes.size() > 1) {
      remote.action = SettingsAction::NextRemote;
      remote.verb = "next remote";
    } else {
      remote.blocked = "only one remote";
    }
  }
  group.rows.push_back(std::move(remote));

  return group;
}

SettingsGroup AppearanceGroup(const SettingsView& view) {
  SettingsGroup group;
  group.title = "APPEARANCE";

  SettingsRow theme_row;
  theme_row.label = "theme";
  theme_row.value = ThemeLabel();
  theme_row.action = SettingsAction::NextTheme;
  theme_row.verb = "next theme";
  group.rows.push_back(std::move(theme_row));

  SettingsRow icons;
  icons.label = "icons";
  icons.value = GlyphModeName(GlyphModeNow());
  // The one setting that can make the screen unreadable if it is guessed wrong,
  // so the page says what "nerd" costs before it is cycled onto rather than
  // leaving the user to work out why every glyph became a box.
  icons.note = "nerd needs a patched font";
  icons.action = SettingsAction::NextIcons;
  icons.verb = "next set";
  group.rows.push_back(std::move(icons));

  SettingsRow logos;
  logos.label = "provider logos";
  logos.value = OnOff(ProviderLogos());
  // Its own row rather than a consequence of the icons row, because it is the
  // one part of the nerd set worth having on a terminal that wants none of the
  // rest: a hexagon is not the GitHub logo, whereas ✓ is a perfectly good check.
  logos.note = "GitHub, GitLab and Actions marks; needs a patched font";
  logos.action = SettingsAction::ToggleLogos;
  logos.verb = "toggle";
  group.rows.push_back(std::move(logos));

  SettingsRow border;
  border.label = "panel border";
  border.value = PanelBorderName(PanelBorderNow());
  border.action = SettingsAction::NextBorder;
  border.verb = "next border";
  group.rows.push_back(std::move(border));

  SettingsRow depth;
  depth.label = "colours";
  depth.value = ColorDepthName(ColorDepthNow());
  // Two palettes can quantize onto the same 256 indices, so a theme switch that
  // appears to do nothing is usually this row rather than a broken key.
  depth.note = ColorDepthNow() == ColorDepth::TrueColor ? "" : "themes differ less below truecolor";
  depth.action = SettingsAction::NextDepth;
  depth.verb = "next depth";
  group.rows.push_back(std::move(depth));

  SettingsRow motion;
  motion.label = "animations";
  motion.value = OnOff(!ReducedMotion());
  // Not "off snaps rather than hides": that sentence sits immediately after the
  // value, so it opens with the word "off" while the value next to it reads
  // "on", and the row contradicts itself at a glance.
  motion.note = "nothing is hidden when they are off";
  motion.action = SettingsAction::ToggleAnimations;
  motion.verb = "toggle";
  group.rows.push_back(std::move(motion));

  SettingsRow compact;
  compact.label = "compact layout";
  compact.value = OnOff(view.compact);
  compact.note = "stack the cards, drop the sidecars";
  compact.action = SettingsAction::ToggleCompact;
  compact.verb = "toggle";
  group.rows.push_back(std::move(compact));

  SettingsRow splash;
  splash.label = "startup card";
  splash.value = OnOff(view.splash);
  splash.note = "takes effect next launch";
  splash.action = SettingsAction::ToggleSplash;
  splash.verb = "toggle";
  group.rows.push_back(std::move(splash));

  return group;
}

SettingsGroup ConfigGroup(const SettingsView& view) {
  SettingsGroup group;
  group.title = "CONFIG";

  SettingsRow save;
  save.label = "save settings";
  save.value = view.dirty ? "changed" : "unchanged";
  // Saving regenerates the file from the keys gittop holds, which is the whole
  // reason this is a key you press rather than something every toggle does on
  // its own: a hand-written config loses its comments to it.
  save.note = view.config_exists ? "rewrites the file; comments are lost" : "creates the file, 0600";
  save.action = SettingsAction::SaveConfig;
  save.verb = "save";
  group.rows.push_back(std::move(save));

  SettingsRow bindings;
  bindings.label = "keys";
  bindings.value = std::to_string(view.key_bindings) + " bindings";
  // A [keys] section that was rejected and one that took effect look identical
  // from the outside, so the count of the ones the config actually moved is the
  // part worth printing.
  bindings.note = view.key_overrides > 0
                      ? std::to_string(view.key_overrides) + " from your config"
                      : "rebind them under [keys]";
  bindings.action = SettingsAction::OpenKeys;
  bindings.verb = "show them";
  group.rows.push_back(std::move(bindings));

  return group;
}

// ------------------------------------------------------------------- the list

Element GroupHeading(const std::string& title) {
  const Theme& t = theme();
  return hbox({
      text(" "),
      text(glyphs().rule) | color(t.accent),
      text(" " + title) | bold | color(t.text_faint),
      filler(),
  });
}

Element Row(const SettingsRow& row, bool selected, int width) {
  const Theme& t = theme();
  const GlyphSet& g = glyphs();

  const bool actionable = row.action != SettingsAction::None;

  // The right-hand column, and only under the cursor: a hint repeated down
  // eleven rows stops being read by the third one. It carries the verb on a row
  // that has one and the reason on a row that does not, which puts the
  // explanation exactly where the missing verb would have been.
  std::string aside;
  if (selected && actionable && !row.verb.empty()) {
    aside = std::string(g.arrow_enter) + " " + row.verb + " ";
  } else if (selected && !row.blocked.empty()) {
    aside = row.blocked + " ";
  }
  const int aside_cells = TextWidth(aside);

  // Fixed columns first, so what the note has left is arithmetic rather than a
  // guess: cursor, gap, label, then whatever the aside claimed on the right.
  const int room = width - 2 - kLabelCells - kSpaceTight - aside_cells - kSpaceTight;
  const int value_cells = TextWidth(row.value);
  const int note_room = room - value_cells - kSpace;

  Elements parts{
      text(selected ? g.cursor : " ") | color(t.accent),
      text(" "),
      text(Fit(row.label, kLabelCells)) | color(selected ? t.text : t.text_dim),
      text(Truncate(row.value, std::max(4, room))) | bold |
          color(actionable ? t.text : t.text_faint),
  };
  // The note stays put whether or not the row can be changed. It is the fact
  // about the setting — which host, which variable — and dropping it to make
  // room for "you cannot change this here" trades the answer for the excuse.
  if (!row.note.empty() && note_room >= 8) {
    parts.push_back(text("  " + Truncate(row.note, note_room)) | color(t.text_faint));
  }
  parts.push_back(filler());
  if (!aside.empty()) {
    parts.push_back(text(aside) | color(actionable ? t.accent : t.text_faint));
  }

  Element line = hbox(std::move(parts));
  if (selected) {
    // focus is what makes the enclosing yframe scroll to it, which is the whole
    // of the short-terminal behaviour: the list keeps the cursor on screen
    // instead of the page running off the bottom with no way to reach it.
    line = std::move(line) | bgcolor(t.surface_alt) | ftxui::focus;
  }
  return line;
}

// ---------------------------------------------------------------- the sidecar

Element Fact(const std::string& label, Element value) {
  const Theme& t = theme();
  constexpr int kFactLabel = 9;
  return hbox({
      Gap(kSpaceTight),
      text(Fit(label, kFactLabel)) | color(t.text_faint),
      std::move(value),
      filler(),
  });
}

Element FactText(const std::string& label, const std::string& value) {
  const Theme& t = theme();
  const int room = kSidecarCells - 2 - kSpaceTight - 9;
  return Fact(label, text(Truncate(value.empty() ? std::string(glyphs().absent) : value, room)) |
                         color(value.empty() ? t.text_faint : t.text));
}

Element RepositoryCard(const SettingsView& view) {
  const Theme& t = theme();
  const GlyphSet& g = glyphs();
  const int room = kSidecarCells - 2 - kSpaceTight - 9;

  Elements rows{
      FactText("name", view.repo_name),
      Fact("path", PathText(view.repo_path, /*emphasised=*/false, room)),
      Fact("branch", hbox({
                         text(std::string(view.head_detached ? g.detached : g.branch) + " ") |
                             color(t.accent),
                         text(Truncate(view.branch, room - 2)) | color(t.text),
                     })),
  };
  if (!view.upstream.empty()) {
    rows.push_back(FactText("upstream", view.upstream));
  }
  return Panel("REPOSITORY", vbox(std::move(rows)), {.focused = false});
}

Element RemotesCard(const SettingsView& view) {
  const Theme& t = theme();
  const GlyphSet& g = glyphs();

  if (view.remotes.empty()) {
    return Panel("REMOTES",
                 hbox({Gap(kSpaceTight), text("none configured") | color(t.text_faint), filler()}),
                 {.focused = false});
  }

  Elements rows;
  rows.reserve(view.remotes.size() * 2);
  for (const SettingsRemote& entry : view.remotes) {
    rows.push_back(hbox({
        Gap(kSpaceTight),
        text(entry.active ? g.node_head : g.bullet) | color(entry.active ? t.accent : t.text_faint),
        text(" " + Truncate(entry.name, kSidecarCells - 6)) |
            color(entry.active ? t.text : t.text_dim),
        filler(),
    }));
    // The URL is already through SafeUrl by the time it reaches this file.
    const std::string mark = ProviderGlyph(entry.provider);
    rows.push_back(hbox({
        Gap(kSpaceWide),
        text(mark + " ") | color(entry.active ? t.accent : t.text_faint),
        text(Truncate(entry.url, kSidecarCells - 2 - kSpaceWide - TextWidth(mark) - 1)) |
            color(t.text_faint),
        filler(),
    }));
  }
  return Panel("REMOTES", vbox(std::move(rows)), {.focused = false});
}

Element ConfigCard(const SettingsView& view) {
  const int room = kSidecarCells - 2 - kSpaceTight - 9;

  Elements rows{
      Fact("config", PathText(view.config_path, /*emphasised=*/true, room)),
      FactText("on disk", view.config_exists ? "yes" : "not yet"),
      FactText("version", "gittop " + view.version),
  };
  return Panel("ABOUT", vbox(std::move(rows)), {.focused = false});
}

}  // namespace

std::vector<SettingsGroup> BuildSettings(const SettingsView& view) {
  return {AccountGroup(view), AppearanceGroup(view), ConfigGroup(view)};
}

int SettingsRowCount(const SettingsView& view) {
  int count = 0;
  for (const SettingsGroup& group : BuildSettings(view)) {
    count += static_cast<int>(group.rows.size());
  }
  return count;
}

SettingsRow SettingsRowAt(const SettingsView& view, int index) {
  if (index < 0) {
    return {};
  }
  int seen = 0;
  for (const SettingsGroup& group : BuildSettings(view)) {
    for (const SettingsRow& row : group.rows) {
      if (seen == index) {
        return row;
      }
      ++seen;
    }
  }
  return {};
}

Element SettingsPanel(const SettingsView& view, int width, std::vector<Box>* rows) {
  const bool wide = width >= kSidecarFloor;
  const int list_width = wide ? width - kSidecarCells : width;

  if (rows != nullptr) {
    rows->assign(static_cast<std::size_t>(SettingsRowCount(view)), Box());
  }

  Elements lines;
  int index = 0;
  bool first = true;
  for (const SettingsGroup& group : BuildSettings(view)) {
    if (!first) {
      lines.push_back(text(""));
    }
    first = false;
    lines.push_back(GroupHeading(group.title));

    for (const SettingsRow& row : group.rows) {
      // Two border columns off the list's width, which is what the row has to
      // fit inside — measured here rather than left to FTXUI, which clips at
      // the frame and says nothing about having done it.
      Element line = Row(row, index == view.selected, list_width - 2);
      if (rows != nullptr) {
        line = std::move(line) | reflect((*rows)[static_cast<std::size_t>(index)]);
      }
      lines.push_back(std::move(line));
      ++index;
    }
  }

  Element list =
      Panel("SETTINGS", Scrollable(vbox(std::move(lines))), {.note = view.dirty ? "unsaved" : ""}) |
      flex;

  if (!wide) {
    // The sidecar is facts about the repository, not settings, so a narrow
    // terminal drops it rather than stacking it under the list and pushing the
    // rows the cursor is on off the bottom.
    return list;
  }

  // RepositoryCard and ConfigCard sit at their content size; the remotes list
  // takes what is left, so the right column is framed all the way down rather
  // than ending in an unframed hole.
  Element sidecar = vbox({
                        RepositoryCard(view),
                        RemotesCard(view) | flex,
                        ConfigCard(view),
                    }) |
                    size(WIDTH, EQUAL, kSidecarCells);

  return hbox({std::move(list), std::move(sidecar)}) | flex;
}

}  // namespace gittop::ui
