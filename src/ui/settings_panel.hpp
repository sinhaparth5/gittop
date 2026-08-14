#pragma once

#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

#include "model/remote.hpp"

namespace gittop::ui {

// What `enter` on a settings row does.
//
// Named rather than positional, and resolved through SettingsRowAt rather than
// through a row index App keeps its own copy of: the list changes shape with
// the state it describes — a signed-in host offers "sign out" where an
// anonymous one offers "connect" — so any second table of "row 3 is the theme"
// would be wrong the first time somebody signed in.
enum class SettingsAction {
  None,
  SignIn,
  SignOut,
  NextRemote,
  NextTheme,
  NextIcons,
  ToggleLogos,
  NextBorder,
  NextDepth,
  ToggleAnimations,
  ToggleCompact,
  ToggleSplash,
  SaveConfig,
  OpenKeys,
};

struct SettingsRow {
  std::string label;
  std::string value;
  std::string note;  // dim, to the right of the value: where it came from
  std::string verb;  // what enter does here, shown on the selected row only

  SettingsAction action = SettingsAction::None;

  // Why this row cannot be changed from here, empty when it can. A row that is
  // merely inert and a row that is inert *for a reason* are different things to
  // look at, and the second one is the only one worth pressing enter on — it
  // answers with this.
  std::string blocked;
};

struct SettingsGroup {
  std::string title;
  std::vector<SettingsRow> rows;
};

// One remote, as the sidecar lists them. `url` arrives already through SafeUrl:
// this header is under ui/ and a remote configured with a token in its URL must
// not reach a panel with the token still in it.
struct SettingsRemote {
  std::string name;
  std::string url;
  // Which host this one points at, so the card can put the provider's mark in
  // front of it. A repository with an origin on one provider and a mirror on
  // the other is otherwise two lines of URL to read.
  model::Provider provider = model::Provider::Unknown;
  bool active = false;
};

// Everything the page needs that App is the only one who knows.
//
// The appearance settings are deliberately *not* in here: theme, glyphs, border
// and colour depth are ui/ state with their own accessors, and copying them
// through App would make this struct a second place they could disagree.
struct SettingsView {
  int selected = 0;

  // ------------------------------------------------------------------ account
  model::Provider provider = model::Provider::Unknown;
  std::string host;
  model::TokenSource token_source = model::TokenSource::None;
  std::string token_origin;  // the variable or the file — never the token

  // --------------------------------------------------------------- repository
  std::string repo_name;
  std::string repo_path;
  std::string branch;
  std::string upstream;
  bool head_detached = false;
  std::vector<SettingsRemote> remotes;

  // ------------------------------------------------------------------- config
  std::string config_path;
  bool config_exists = false;
  // A setting has been changed this session. Deliberately not "differs from the
  // file": gittop cannot know what the file would produce without re-reading and
  // re-resolving every `auto`, and a claim it cannot check is worse than one it
  // can.
  bool dirty = false;
  int key_bindings = 0;
  int key_overrides = 0;  // how many of them the config moved

  // Toggles App owns, because they are read outside ui/.
  bool compact = false;
  bool splash = true;

  std::string version;
};

// The one place the list is built, and the reason `enter` on row four always
// acts on the setting row four is showing. App calls it to count and to resolve
// a keystroke; the panel calls it to draw. Two tables would be two tables to
// keep in step.
std::vector<SettingsGroup> BuildSettings(const SettingsView& view);

// Rows only — group headings are not landing places for the cursor.
int SettingsRowCount(const SettingsView& view);

// The row at a flat index across every group, in reading order. A default row
// (action None, empty label) for an index outside the list.
SettingsRow SettingsRowAt(const SettingsView& view, int index);

// `rows` is filled during layout with the box each row landed in, so a click can
// be turned back into a row. Same arrangement as every other list here: FTXUI
// computes geometry only while rendering, so reflect() is the only way to learn
// it.
//
// No height parameter: the list scrolls to the cursor inside its own frame, so
// how many rows fit is a question the layout answers rather than one the caller
// has to.
ftxui::Element SettingsPanel(const SettingsView& view, int width,
                             std::vector<ftxui::Box>* rows = nullptr);

}  // namespace gittop::ui
