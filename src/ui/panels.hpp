#pragma once

#include <cstdint>
#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

#include "model/operation.hpp"
#include "model/status.hpp"
#include "ui/keymap.hpp"

namespace gittop::ui {

// Bar fill levels, kept apart from the snapshot because they ease toward the
// real counts across a few frames while the printed numbers stay exact. An
// animated digit is unreadable; an animated bar is the part worth smoothing.
struct StatBars {
  float staged = 0.0F;
  float unstaged = 0.0F;
  float untracked = 0.0F;
  float conflicted = 0.0F;
};

enum class View {
  Status,
  History,
  Branches,
  Graph,
  Diff,
  Stashes,
  Remote,
  Pipelines,
  Pulls,
};

// Tab-bar order, which is also the order `tab` cycles, the order the tab
// hit-boxes come back in, and — since the digit keys became positional — what
// `1` through `9` reach. One list rather than a switch in each of those places,
// which is where the last view-adding bug came from.
const std::vector<View>& AllViews();

// Replaces the tab set and its order, for `[layout] views`. Refuses an empty
// list: a config typo must not be able to produce a program with no views in
// it, and the built-in order is a better answer than a blank tab bar.
bool SetViews(const std::vector<View>& views);

// The spelling a config uses, and back. False on a name that is not a view, so
// ApplyConfig can name it rather than dropping the line.
bool ParseViewName(const std::string& name, View* out);
std::string ViewName(View view);

ftxui::Element Header(const model::StatusSnapshot& snapshot);

// `tabs` is filled during layout with the box each tab landed in, so a click can
// be turned back into a view. FTXUI computes a node's geometry only while
// rendering it, so reflect() is the only way to learn it.
ftxui::Element TabBar(View active, std::vector<ftxui::Box>* tabs = nullptr);

// `compact` stacks the four cards two-by-two, for terminals too narrow to give
// each one a readable bar side by side.
ftxui::Element SummaryRow(const model::StatusSnapshot& snapshot, const StatBars& bars,
                          bool compact);
ftxui::Element FileList(const model::StatusSnapshot& snapshot, int selected,
                        std::vector<ftxui::Box>* rows = nullptr);

// `fade` runs 1 down to 0 as a message ages out. The keymap is read rather than
// captured in string literals, so a rebound key shows up in the hints. `filter`
// is the live search, echoed on the key row so a list that is hiding rows never
// looks like a list that has none.
ftxui::Element Footer(const std::string& message, bool is_error, float fade, View view,
                      const Keymap& keys, const std::string& filter);

// Lays out in two columns whenever the terminal is wide enough and too short
// for one, which is most of them: the single column is forty-odd rows and gets
// its bottom half clipped away with nothing on screen saying there was more.
// `scroll` is the row to keep in view, and only matters in the one-column
// fallback — a terminal too narrow for two columns and too short for one.
ftxui::Element HelpPane(const Keymap& keys, int width, int height, int scroll);

// A strip above the file list saying what the repository is in the middle of.
// Returns nothing at all when it is in the middle of nothing, so the caller can
// push it unconditionally and the status view keeps its full height.
ftxui::Element OperationBanner(const model::OperationState& state, const Keymap& keys);

// The continue-or-abort dialog. Separate from ConfirmPane because it is two
// destructive answers rather than one, and folding a second verb into a
// yes/no pane is how `y` ends up meaning different things on different days.
ftxui::Element OperationPane(const model::OperationState& state);

// The search box. Takes the already-rendered input for the same reason the
// passphrase pane does — not secrecy here, just the one way this file gets a
// live Input without depending on the component layer.
ftxui::Element FilterPane(ftxui::Element input, const std::string& scope, int matches,
                          int total);

// `warning` is the line in danger colour under the detail, empty for a question
// that is merely worth asking. `confirm_label` names what `y` does.
ftxui::Element ConfirmPane(const std::string& question, const std::string& detail,
                           const std::string& warning, const std::string& confirm_label);

// A transfer in flight, translated out of git::TransferProgress by App so this
// header stays free of libgit2. `ratio` is -1 while the totals are unknown,
// which draws an indeterminate bar rather than a made-up percentage.
struct TransferView {
  std::string title;
  std::string phase;
  std::string detail;
  float ratio = -1.0F;
  int objects = 0;
  int total = 0;
  std::int64_t bytes = 0;
  bool cancellable = true;
};

ftxui::Element TransferPane(const TransferView& view, int frame);

// The ssh key passphrase prompt.
//
// Takes the already-rendered input rather than the string behind it, which is
// how the rule that nothing under ui/ handles a secret survives a panel whose
// entire job is to collect one: this function cannot read what was typed, only
// place the box it was typed into. The Input itself is in password mode, so
// what is on screen is asterisks in the first place.
ftxui::Element PassphrasePane(ftxui::Element input, bool rejected);

ftxui::Decorator PaneFrame();

}  // namespace gittop::ui
