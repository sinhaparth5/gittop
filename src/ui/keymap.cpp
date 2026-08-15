#include "ui/keymap.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace gittop::ui {
namespace {

using ftxui::Event;

struct NamedKey {
  const char* name;
  const Event* event;
};

// Addresses of FTXUI's own constants rather than copies, so a key gittop calls
// "enter" is the same object the library compares against.
const NamedKey kNamedKeys[] = {
    {"enter", &Event::Return},      {"return", &Event::Return},
    {"esc", &Event::Escape},        {"escape", &Event::Escape},
    {"tab", &Event::Tab},           {"backtab", &Event::TabReverse},
    {"up", &Event::ArrowUp},        {"down", &Event::ArrowDown},
    {"left", &Event::ArrowLeft},    {"right", &Event::ArrowRight},
    {"home", &Event::Home},         {"end", &Event::End},
    {"pageup", &Event::PageUp},     {"pagedown", &Event::PageDown},
    {"insert", &Event::Insert},     {"delete", &Event::Delete},
    {"backspace", &Event::Backspace},
    {"f1", &Event::F1},             {"f2", &Event::F2},
    {"f3", &Event::F3},             {"f4", &Event::F4},
    {"f5", &Event::F5},             {"f6", &Event::F6},
    {"f7", &Event::F7},             {"f8", &Event::F8},
    {"f9", &Event::F9},             {"f10", &Event::F10},
    {"f11", &Event::F11},           {"f12", &Event::F12},
};

struct NamedAction {
  const char* name;
  Action action;
};

// The spelling a config file uses. Kept next to the enum rather than derived
// from it, because these are user-facing names that should be free to differ
// from the identifier: `view_ci` reads better in a config than `view_pipelines`.
const NamedAction kActionNames[] = {
    {"quit", Action::Quit},
    {"help", Action::Help},
    {"reload", Action::Reload},
    {"theme", Action::Theme},

    {"next_view", Action::NextView},
    {"prev_view", Action::PrevView},
    {"view_1", Action::View1},
    {"view_2", Action::View2},
    {"view_3", Action::View3},
    {"view_4", Action::View4},
    {"view_5", Action::View5},
    {"view_6", Action::View6},
    {"view_7", Action::View7},
    {"view_8", Action::View8},
    {"view_9", Action::View9},
    {"view_10", Action::View10},

    {"down", Action::Down},
    {"up", Action::Up},
    {"first", Action::First},
    {"last", Action::Last},
    {"page_down", Action::PageDown},
    {"page_up", Action::PageUp},
    {"open", Action::Open},
    {"filter", Action::Filter},

    {"next_remote", Action::NextRemote},
    {"sign_in", Action::SignIn},
    {"fetch", Action::Fetch},
    {"pull", Action::Pull},
    {"push", Action::Push},

    {"toggle_stage", Action::ToggleStage},
    {"stage", Action::Stage},
    {"unstage", Action::Unstage},
    {"stage_all", Action::StageAll},
    {"discard", Action::Discard},
    {"commit", Action::Commit},

    {"diff_switch", Action::DiffSwitch},
    {"diff_next_file", Action::DiffNextFile},
    {"diff_prev_file", Action::DiffPrevFile},

    {"stash_save", Action::StashSave},
    {"stash_apply", Action::StashApply},
    {"stash_pop", Action::StashPop},
    {"stash_drop", Action::StashDrop},
    {"prune", Action::Prune},
    {"ci_ref", Action::CiRef},
    {"pull_create", Action::PullCreate},

    {"rebase", Action::Rebase},
    {"operation", Action::Operation},

    {"pan_left", Action::PanLeft},
    {"pan_right", Action::PanRight},
    {"bucket_day", Action::BucketDay},
    {"bucket_week", Action::BucketWeek},
    {"bucket_month", Action::BucketMonth},
    {"graph_oldest", Action::GraphOldest},
    {"graph_newest", Action::GraphNewest},
};

std::string ScopeName(Scope scope) {
  switch (scope) {
    case Scope::Status:
      return "status";
    case Scope::Graph:
      return "graph";
    case Scope::Diff:
      return "diff";
    case Scope::Stash:
      return "stash";
    case Scope::Branches:
      return "branches";
    case Scope::Ci:
      return "ci";
    case Scope::Pull:
      return "pulls";
    case Scope::Global:
      break;
  }
  return "global";
}

std::string Lower(const std::string& text) {
  std::string out = text;
  for (char& ch : out) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return out;
}

std::vector<std::string> Split(const std::string& text) {
  std::vector<std::string> parts;
  std::string current;
  for (const char ch : text) {
    if (ch == ' ' || ch == '\t' || ch == ',') {
      if (!current.empty()) {
        parts.push_back(std::move(current));
        current.clear();
      }
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty()) {
    parts.push_back(std::move(current));
  }
  return parts;
}

}  // namespace

bool ParseKey(const std::string& spec, Event* event, std::string* label) {
  if (spec.empty()) {
    return false;
  }

  // A bare printable character is itself, and case matters: `g` and `G` are two
  // different bindings, which is why this runs before any lowercasing.
  if (spec.size() == 1 && spec[0] >= 0x20 && spec[0] < 0x7f) {
    *event = Event::Character(spec[0]);
    *label = spec;
    return true;
  }

  const std::string key = Lower(spec);
  if (key == "space") {
    *event = Event::Character(' ');
    *label = "space";
    return true;
  }

  // Ctrl and Alt are the two prefixes worth supporting. FTXUI encodes them as
  // the control character and as ESC-prefixed input respectively, which is what
  // a terminal actually sends, so they are built here rather than looked up.
  const auto prefixed = [&key](const char* prefix) {
    const std::size_t n = std::string(prefix).size();
    return key.size() > n && key.compare(0, n, prefix) == 0;
  };
  if (prefixed("ctrl-") || prefixed("c-")) {
    const std::string rest = key.substr(key[1] == '-' ? 2 : 5);
    if (rest.size() != 1 || rest[0] < 'a' || rest[0] > 'z') {
      return false;
    }
    *event = Event::Special(std::string(1, static_cast<char>(rest[0] - 'a' + 1)));
    *label = "ctrl-" + rest;
    return true;
  }
  if (prefixed("alt-") || prefixed("m-")) {
    const std::string rest = key.substr(key[1] == '-' ? 2 : 4);
    if (rest.size() != 1) {
      return false;
    }
    *event = Event::Special(std::string("\x1b") + rest);
    *label = "alt-" + rest;
    return true;
  }

  for (const NamedKey& named : kNamedKeys) {
    if (key == named.name) {
      *event = *named.event;
      *label = named.name;
      return true;
    }
  }
  return false;
}

Keymap::Keymap() {
  Bind(Action::Quit, Scope::Global, "q esc");
  Bind(Action::Help, Scope::Global, "?");
  Bind(Action::Reload, Scope::Global, "r");
  Bind(Action::Theme, Scope::Global, "t");

  Bind(Action::NextView, Scope::Global, "tab");
  Bind(Action::PrevView, Scope::Global, "backtab");
  Bind(Action::View1, Scope::Global, "1");
  Bind(Action::View2, Scope::Global, "2");
  Bind(Action::View3, Scope::Global, "3");
  Bind(Action::View4, Scope::Global, "4");
  Bind(Action::View5, Scope::Global, "5");
  Bind(Action::View6, Scope::Global, "6");
  Bind(Action::View7, Scope::Global, "7");
  Bind(Action::View8, Scope::Global, "8");
  Bind(Action::View9, Scope::Global, "9");
  Bind(Action::View10, Scope::Global, "0");

  Bind(Action::Down, Scope::Global, "j down");
  Bind(Action::Up, Scope::Global, "k up");
  Bind(Action::First, Scope::Global, "g home");
  Bind(Action::Last, Scope::Global, "G end");
  Bind(Action::PageDown, Scope::Global, "ctrl-d pagedown");
  Bind(Action::PageUp, Scope::Global, "ctrl-u pageup");
  Bind(Action::Open, Scope::Global, "enter");
  Bind(Action::Filter, Scope::Global, "/");

  Bind(Action::NextRemote, Scope::Global, "R");
  // Global rather than scoped to the Remote view, even though that is where the
  // "anonymous" line that prompts it appears: the thing a sign-in fixes is a
  // 404 on a private repository, and those turn up on the CI and pull request
  // views just as often.
  Bind(Action::SignIn, Scope::Global, "L");
  Bind(Action::Fetch, Scope::Global, "f");
  Bind(Action::Pull, Scope::Global, "p");
  Bind(Action::Push, Scope::Global, "P");

  // Stashing has to work from the view where you notice you need it, which is
  // the status view, so saving is global and only the three keys that act on a
  // selected entry are scoped to the list that has one.
  Bind(Action::StashSave, Scope::Global, "S");
  Bind(Action::Rebase, Scope::Global, "B");
  Bind(Action::Operation, Scope::Global, "o");

  Bind(Action::ToggleStage, Scope::Status, "space");
  Bind(Action::Stage, Scope::Status, "s");
  Bind(Action::Unstage, Scope::Status, "u");
  Bind(Action::StageAll, Scope::Status, "a");
  Bind(Action::Discard, Scope::Status, "d");
  Bind(Action::Commit, Scope::Status, "c");

  // `s` switches sides on the diff view and stages on the status view: two
  // scoped bindings that never meet, so neither shadows the other.
  Bind(Action::DiffSwitch, Scope::Diff, "s");
  Bind(Action::DiffNextFile, Scope::Diff, "]");
  Bind(Action::DiffPrevFile, Scope::Diff, "[");

  // `p` here shadows the global pull, deliberately: a pull is not what anyone
  // means by `p` while looking at a list of stashes.
  Bind(Action::StashApply, Scope::Stash, "a");
  Bind(Action::StashPop, Scope::Stash, "p");
  Bind(Action::StashDrop, Scope::Stash, "d");

  // Scoped to the one view that shows what it would remove. A global prune
  // key would be a network call with a destructive edge available from every
  // screen, including several that have nothing to do with branches.
  Bind(Action::Prune, Scope::Branches, "x");

  // Scoped for the same reason, from the other direction: the ref picker only
  // means anything beside a run list, and `b` is a letter worth leaving free
  // everywhere else.
  Bind(Action::CiRef, Scope::Ci, "b");

  // Scoped to the view that lists them, for the third time and the same reason:
  // this is the one screen that shows whether one is already open for the
  // branch, which is the question worth having answered before opening another.
  // `n` is unbound everywhere else, so nothing is shadowed by it.
  Bind(Action::PullCreate, Scope::Pull, "n");

  // The graph pans along a timeline instead of selecting rows, so it takes the
  // horizontal keys and reuses g/G for the ends of time rather than the ends of
  // a list. Both are shadows of a global binding, which is what Scope is for.
  Bind(Action::PanLeft, Scope::Graph, "h left");
  Bind(Action::PanRight, Scope::Graph, "l right");
  Bind(Action::BucketDay, Scope::Graph, "d");
  Bind(Action::BucketWeek, Scope::Graph, "w");
  Bind(Action::BucketMonth, Scope::Graph, "m");
  Bind(Action::GraphOldest, Scope::Graph, "g");
  Bind(Action::GraphNewest, Scope::Graph, "G");
}

void Keymap::Bind(Action action, Scope scope, const char* spec) {
  for (const std::string& part : Split(spec)) {
    Binding binding;
    binding.action = action;
    binding.scope = scope;
    if (ParseKey(part, &binding.event, &binding.label)) {
      bindings_.push_back(std::move(binding));
    }
  }
}

bool Keymap::Rebind(const std::string& action_name, const std::string& spec, std::string* error) {
  Action action = Action::None;
  Scope scope = Scope::Global;
  bool found = false;
  for (const NamedAction& named : kActionNames) {
    if (action_name == named.name) {
      action = named.action;
      found = true;
      break;
    }
  }
  if (!found) {
    *error = "unknown action '" + action_name + "'";
    return false;
  }

  // The action keeps the scope its defaults had. Moving `discard` to another
  // key should not also move it out of the status view, and there is no syntax
  // for saying otherwise on purpose: a scope is a fact about what the action
  // does, not a preference.
  for (const Binding& binding : bindings_) {
    if (binding.action == action) {
      scope = binding.scope;
      break;
    }
  }

  std::vector<Binding> parsed;
  for (const std::string& part : Split(spec)) {
    Binding binding;
    binding.action = action;
    binding.scope = scope;
    binding.user_set = true;
    if (!ParseKey(part, &binding.event, &binding.label)) {
      *error = "'" + part + "' is not a key gittop knows";
      return false;
    }
    parsed.push_back(std::move(binding));
  }

  // Only applied once every key in the spec parsed, so a line with one typo in
  // it leaves the old binding alone rather than half-applying.
  bindings_.erase(std::remove_if(bindings_.begin(), bindings_.end(),
                                 [action](const Binding& b) { return b.action == action; }),
                  bindings_.end());
  for (Binding& binding : parsed) {
    bindings_.push_back(std::move(binding));
  }
  return true;
}

Action Keymap::Lookup(Scope scope, const Event& event) const {
  if (scope != Scope::Global) {
    for (const Binding& binding : bindings_) {
      if (binding.scope == scope && binding.event == event) {
        return binding.action;
      }
    }
  }
  for (const Binding& binding : bindings_) {
    if (binding.scope == Scope::Global && binding.event == event) {
      return binding.action;
    }
  }
  return Action::None;
}

std::string Keymap::KeyFor(Action action) const {
  for (const Binding& binding : bindings_) {
    if (binding.action == action) {
      return binding.label;
    }
  }
  return {};
}

std::vector<std::string> Keymap::KeysFor(Action action) const {
  std::vector<std::string> keys;
  for (const Binding& binding : bindings_) {
    if (binding.action == action) {
      keys.push_back(binding.label);
    }
  }
  return keys;
}

std::vector<std::string> Keymap::Conflicts() const {
  std::vector<std::string> out;
  for (std::size_t i = 0; i < bindings_.size(); ++i) {
    for (std::size_t j = i + 1; j < bindings_.size(); ++j) {
      const Binding& a = bindings_[i];
      const Binding& b = bindings_[j];
      if (a.action == b.action || !(a.event == b.event)) {
        continue;
      }
      // Two scoped bindings in different views never meet.
      const bool both_scoped = a.scope != Scope::Global && b.scope != Scope::Global;
      if (both_scoped && a.scope != b.scope) {
        continue;
      }
      if (a.scope == b.scope) {
        out.push_back(a.label + " is bound to both " + ActionName(a.action) + " and " +
                      ActionName(b.action));
        continue;
      }
      // A scoped key on top of a global one is how the graph takes g and G for
      // the ends of time, so the built-in arrangement is not a conflict — it is
      // the feature. It becomes one the moment the config is what put the two
      // on the same key, because then nobody chose the shadowing on purpose.
      if (!a.user_set && !b.user_set) {
        continue;
      }
      // Lookup tries the view's own scope first, so the scoped one is the one
      // that runs and the global one is the one that goes missing. Naming which
      // is which is the whole value of saying anything at all.
      const Binding& scoped = a.scope == Scope::Global ? b : a;
      const Binding& shadowed = a.scope == Scope::Global ? a : b;
      out.push_back(ActionName(shadowed.action) + " is unreachable on the " +
                    ScopeName(scoped.scope) + " view: " + scoped.label + " is bound to " +
                    ActionName(scoped.action) + " there");
    }
  }
  return out;
}

int Keymap::UserCount() const {
  int count = 0;
  for (const Binding& binding : bindings_) {
    if (binding.user_set) {
      ++count;
    }
  }
  return count;
}

std::string Keymap::ActionName(Action action) {
  for (const NamedAction& named : kActionNames) {
    if (named.action == action) {
      return named.name;
    }
  }
  return "none";
}

}  // namespace gittop::ui
