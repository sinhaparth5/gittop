#pragma once

#include <cstdint>
#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

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
  Remote,
  Pipelines,
  Pulls,
};

// Tab-bar order, which is also the order `tab` cycles and the order the tab
// hit-boxes come back in. One list rather than a switch in each of those three
// places, which is where the last view-adding bug came from.
const std::vector<View>& AllViews();

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
// captured in string literals, so a rebound key shows up in the hints.
ftxui::Element Footer(const std::string& message, bool is_error, float fade, View view,
                      const Keymap& keys);

ftxui::Element HelpPane(const Keymap& keys);

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

ftxui::Decorator PaneFrame();

}  // namespace gittop::ui
