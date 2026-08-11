#pragma once

#include <ftxui/component/event.hpp>
#include <string>
#include <vector>

namespace gittop::ui {

// Everything a key can ask for. Routing compares against one of these rather
// than against an Event literal, which is what lets the config move a key and
// what lets the footer and the help overlay print the binding actually in force
// instead of the one that was true when the string was typed.
enum class Action {
  None,

  Quit,
  Help,
  Reload,
  Theme,

  NextView,
  PrevView,
  // Positional rather than named, because `[layout] views` lets the config
  // reorder the tab bar and a digit that jumps to a fixed view would then
  // disagree with the digit printed on the tab. A slot past the end of the
  // configured list does nothing.
  View1,
  View2,
  View3,
  View4,
  View5,
  View6,
  View7,
  View8,
  View9,

  Down,
  Up,
  First,
  Last,
  PageDown,
  PageUp,
  Open,
  Filter,

  NextRemote,
  Fetch,
  Pull,
  Push,

  ToggleStage,
  Stage,
  Unstage,
  StageAll,
  Discard,
  Commit,

  DiffSwitch,
  DiffNextFile,
  DiffPrevFile,

  StashSave,
  StashApply,
  StashPop,
  StashDrop,

  Rebase,
  Operation,

  PanLeft,
  PanRight,
  BucketDay,
  BucketWeek,
  BucketMonth,
  GraphOldest,
  GraphNewest,
};

// Which keys are live depends on what is on screen: `d` discards a file on the
// status view and switches the graph to daily buckets on the graph view. Scope
// is a property of the default binding rather than of the config syntax, so a
// user rebinds `discard` without having to know that it only exists in one
// place.
enum class Scope {
  Global,
  Status,
  Graph,
  Diff,
  Stash,
};

struct Binding {
  Action action = Action::None;
  Scope scope = Scope::Global;
  ftxui::Event event;
  std::string label;  // the canonical spelling: "j", "enter", "ctrl-d"

  // Whether the config put it here. A scoped key sitting on top of a global one
  // is the shadowing mechanism when gittop arranged it and a surprise when the
  // user did, and there is no way to tell those apart without remembering this.
  bool user_set = false;
};

// A key as a person writes it in a config file: a single character, or one of
// the names below, optionally prefixed with ctrl- or alt-.
//
//   enter return esc escape tab backtab space up down left right
//   home end pageup pagedown insert delete backspace f1 … f12
//
// Returns false on anything else, so a typo is reported at startup rather than
// quietly leaving an action unreachable.
bool ParseKey(const std::string& spec, ftxui::Event* event, std::string* label);

class Keymap {
 public:
  Keymap();  // the built-in bindings

  // Replaces every key bound to `action_name` with the space-separated keys in
  // `spec`. An empty spec unbinds the action entirely, which is a legitimate
  // thing to want: it is how you take `q` away from a fat-fingered quit.
  bool Rebind(const std::string& action_name, const std::string& spec, std::string* error);

  // Scope before Global, so the graph's `d` shadows the shared one the same way
  // the hand-written routing used to.
  Action Lookup(Scope scope, const ftxui::Event& event) const;

  // The first key bound to an action, for a footer chip or a help row. Empty
  // when the action has been unbound.
  std::string KeyFor(Action action) const;
  std::vector<std::string> KeysFor(Action action) const;

  // Keys that two actions reachable at the same moment both claim — same scope,
  // or a scoped binding sitting on top of a global one. Reported once at
  // startup, because the alternative is a user discovering it by pressing the
  // key and getting the wrong thing.
  std::vector<std::string> Conflicts() const;

  static std::string ActionName(Action action);

 private:
  void Bind(Action action, Scope scope, const char* spec);

  std::vector<Binding> bindings_;
};

}  // namespace gittop::ui
