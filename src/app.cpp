#include "app.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <utility>

#include "ui/panels.hpp"
#include "ui/theme.hpp"

namespace gittop {

using namespace ftxui;  // NOLINT: the component DSL reads badly when qualified.

App::App(git::Repository repo) : repo_(std::move(repo)) {}

void App::Refresh() {
  snapshot_ = repo_.ReadStatus();

  const int count = static_cast<int>(snapshot_.entries.size());
  selected_ = std::clamp(selected_, 0, std::max(0, count - 1));
}

void App::Note(std::string message, bool is_error) {
  message_ = std::move(message);
  message_is_error_ = is_error;
}

void App::Apply(const git::OpResult& result) {
  Note(result.message, !result.ok);
  if (result.ok) {
    Refresh();
  }
}

void App::Move(int delta) {
  const int count = static_cast<int>(snapshot_.entries.size());
  if (count == 0) {
    return;
  }
  selected_ = std::clamp(selected_ + delta, 0, count - 1);
}

void App::SelectFirst() {
  selected_ = 0;
}

void App::SelectLast() {
  selected_ = std::max(0, static_cast<int>(snapshot_.entries.size()) - 1);
}

const model::StatusEntry* App::Selected() const {
  if (selected_ < 0 || selected_ >= static_cast<int>(snapshot_.entries.size())) {
    return nullptr;
  }
  return &snapshot_.entries[static_cast<std::size_t>(selected_)];
}

void App::ToggleStage() {
  const model::StatusEntry* entry = Selected();
  if (entry == nullptr) {
    return;
  }
  if (entry->stage == model::Stage::Index) {
    Apply(repo_.Unstage(*entry));
  } else {
    Apply(repo_.Stage(*entry));
  }
}

void App::StageSelected() {
  const model::StatusEntry* entry = Selected();
  if (entry == nullptr) {
    return;
  }
  if (entry->stage == model::Stage::Index) {
    Note("already staged", false);
    return;
  }
  Apply(repo_.Stage(*entry));
}

void App::UnstageSelected() {
  const model::StatusEntry* entry = Selected();
  if (entry == nullptr) {
    return;
  }
  if (entry->stage != model::Stage::Index) {
    Note("that change is not staged", false);
    return;
  }
  Apply(repo_.Unstage(*entry));
}

void App::StageEverything() {
  if (snapshot_.clean()) {
    Note("nothing to stage", false);
    return;
  }
  Apply(repo_.StageAll());
}

void App::RequestDiscard() {
  const model::StatusEntry* entry = Selected();
  if (entry == nullptr) {
    return;
  }
  if (entry->stage == model::Stage::Index) {
    Note("unstage this first, then discard", true);
    return;
  }
  discard_target_ = *entry;
  OpenOverlay(kConfirm);
}

void App::PerformDiscard() {
  CloseOverlay();
  if (discard_target_.path.empty()) {
    return;
  }
  Apply(repo_.Discard(discard_target_));
  discard_target_ = {};
}

void App::OpenCommit() {
  if (snapshot_.staged == 0) {
    Note("nothing staged to commit", true);
    return;
  }
  commit_message_.clear();
  OpenOverlay(kCommit);
}

void App::PerformCommit() {
  if (commit_message_.empty()) {
    Note("commit message is empty", true);
    return;
  }
  const git::OpResult result = repo_.Commit(commit_message_);
  if (result.ok) {
    commit_message_.clear();
    CloseOverlay();
  }
  Apply(result);
}

void App::OpenOverlay(Overlay which) {
  overlay_index_ = which;
  overlay_open_ = true;
}

void App::CloseOverlay() {
  overlay_open_ = false;
}

int App::Run() {
  Refresh();

  auto screen = ScreenInteractive::Fullscreen();

  // ---------------------------------------------------------- commit overlay
  InputOption input_option;
  input_option.multiline = false;
  input_option.on_enter = [this] { PerformCommit(); };
  auto commit_input = Input(&commit_message_, "summary of the change", input_option);

  auto commit_pane = Renderer(commit_input, [this, commit_input] {
    return vbox({
               text(" Commit") | bold | color(ui::theme().accent),
               separator() | color(ui::theme().border),
               hbox({
                   text("  ❯ ") | color(ui::theme().staged),
                   commit_input->Render() | flex,
               }),
               separator() | color(ui::theme().border),
               hbox({
                   ui::KeyCap("enter"),
                   text("commit  ") | color(ui::theme().text_faint),
                   ui::KeyCap("esc"),
                   text("cancel") | color(ui::theme().text_faint),
               }),
           }) |
           ui::PaneFrame() | size(WIDTH, GREATER_THAN, 56);
  });

  commit_pane |= CatchEvent([this](const Event& event) {
    if (event == Event::Escape) {
      CloseOverlay();
      return true;
    }
    return false;
  });

  // --------------------------------------------------------- confirm overlay
  auto confirm_pane = Renderer([this] {
    return ui::ConfirmPane(
        discard_target_.change == model::Change::Untracked ? "Delete this file?"
                                                           : "Discard these changes?",
        discard_target_.path);
  });

  confirm_pane |= CatchEvent([this](const Event& event) {
    if (event == Event::Character('y') || event == Event::Character('Y')) {
      PerformDiscard();
      return true;
    }
    if (event == Event::Character('n') || event == Event::Character('N') ||
        event == Event::Escape) {
      CloseOverlay();
      return true;
    }
    return true;  // Swallow everything else so a stray key cannot destroy work.
  });

  // ------------------------------------------------------------ help overlay
  auto help_pane = Renderer([] { return ui::HelpPane(); });
  help_pane |= CatchEvent([this](const Event& event) {
    if (event.is_character() || event == Event::Escape) {
      CloseOverlay();
      return true;
    }
    return false;
  });

  auto overlay = Container::Tab({commit_pane, confirm_pane, help_pane}, &overlay_index_);

  // --------------------------------------------------------------- main view
  auto main_view = Renderer([this] {
    return vbox({
               ui::Header(snapshot_),
               ui::SummaryRow(snapshot_),
               ui::FileList(snapshot_, selected_) | flex,
               ui::Footer(message_, message_is_error_),
           }) |
           bgcolor(ui::theme().bg);
  });

  auto root = main_view | Modal(overlay, &overlay_open_);

  // This handler is attached outside the modal wrapper on purpose.
  // Container::Stacked delivers events to its focused child, and the overlay
  // holds the only focusable component in the tree (the commit Input), so a
  // handler attached to main_view stops receiving keys the moment a modal is
  // wrapped around it. Returning false while an overlay is open lets the event
  // fall through to that overlay.
  root |= CatchEvent([this, &screen](const Event& event) {
    if (overlay_open_) {
      return false;
    }

    if (event == Event::Character('q') || event == Event::Escape) {
      screen.Exit();
      return true;
    }
    if (event == Event::Character('j') || event == Event::ArrowDown) {
      Move(1);
      return true;
    }
    if (event == Event::Character('k') || event == Event::ArrowUp) {
      Move(-1);
      return true;
    }
    if (event == Event::Character('g') || event == Event::Home) {
      SelectFirst();
      return true;
    }
    if (event == Event::Character('G') || event == Event::End) {
      SelectLast();
      return true;
    }
    if (event == Event::Character(' ')) {
      ToggleStage();
      return true;
    }
    if (event == Event::Character('s')) {
      StageSelected();
      return true;
    }
    if (event == Event::Character('u')) {
      UnstageSelected();
      return true;
    }
    if (event == Event::Character('a')) {
      StageEverything();
      return true;
    }
    if (event == Event::Character('d')) {
      RequestDiscard();
      return true;
    }
    if (event == Event::Character('c')) {
      OpenCommit();
      return true;
    }
    if (event == Event::Character('r')) {
      Refresh();
      Note("re-read the repository", false);
      return true;
    }
    if (event == Event::Character('?')) {
      OpenOverlay(kHelp);
      return true;
    }
    return false;
  });

  screen.Loop(root);
  return 0;
}

}  // namespace gittop
