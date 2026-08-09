#include "app.hpp"

#include <ftxui/component/animation.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

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
  message_at_ = std::chrono::steady_clock::now();
}

ui::StatBars App::TargetBars() const {
  const auto total = static_cast<float>(snapshot_.total());
  if (total <= 0.0F) {
    return {};
  }
  return ui::StatBars{
      static_cast<float>(snapshot_.staged) / total,
      static_cast<float>(snapshot_.unstaged) / total,
      static_cast<float>(snapshot_.untracked) / total,
      static_cast<float>(snapshot_.conflicted) / total,
  };
}

void App::Tick() {
  const auto now = std::chrono::steady_clock::now();
  if (last_frame_.time_since_epoch().count() == 0) {
    last_frame_ = now;
  }
  // Clamped so a frame delayed by a slow status read does not make the bars
  // jump the whole distance at once.
  const float dt =
      std::clamp(std::chrono::duration<float>(now - last_frame_).count(), 0.0F, 0.1F);
  last_frame_ = now;

  // Exponential approach with an 80ms time constant covers ~95% of the distance
  // in 240ms, which lands in the window that reads as responsive instead of
  // either instant or sluggish.
  constexpr float kTau = 0.08F;
  const float k = 1.0F - std::exp(-dt / kTau);

  const ui::StatBars target = TargetBars();
  const auto approach = [k](float& value, float goal) {
    value += (goal - value) * k;
    if (std::abs(goal - value) < 0.002F) {
      value = goal;
    }
  };
  approach(bars_.staged, target.staged);
  approach(bars_.unstaged, target.unstaged);
  approach(bars_.untracked, target.untracked);
  approach(bars_.conflicted, target.conflicted);
}

float App::ToastFade() const {
  if (message_.empty()) {
    return 0.0F;
  }
  constexpr float kHold = 3.0F;
  constexpr float kFade = 0.7F;
  const float age =
      std::chrono::duration<float>(std::chrono::steady_clock::now() - message_at_).count();

  if (age <= kHold) {
    return 1.0F;
  }
  if (age >= kHold + kFade) {
    return 0.0F;
  }
  return 1.0F - ((age - kHold) / kFade);
}

bool App::Animating() const {
  const ui::StatBars target = TargetBars();
  const auto moving = [](float a, float b) { return std::abs(a - b) > 0.0005F; };
  if (moving(bars_.staged, target.staged) || moving(bars_.unstaged, target.unstaged) ||
      moving(bars_.untracked, target.untracked) ||
      moving(bars_.conflicted, target.conflicted)) {
    return true;
  }
  // Keeps frames coming through the toast's hold so the fade actually starts
  // when it should. Bounded at a few seconds, then the screen goes quiet again.
  return !message_.empty() && ToastFade() > 0.0F;
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
                   text(" "),
                   ui::Chip("enter", "commit"),
                   filler(),
                   ui::Chip("esc", "cancel"),
               }),
           }) |
           ui::PaneFrame() | size(WIDTH, GREATER_THAN, 60);
  });

  // --------------------------------------------------------- confirm overlay
  auto confirm_pane = Renderer([this] {
    return ui::ConfirmPane(
        discard_target_.change == model::Change::Untracked ? "Delete this file?"
                                                           : "Discard these changes?",
        discard_target_.path);
  });

  // ------------------------------------------------------------ help overlay
  auto help_pane = Renderer([] { return ui::HelpPane(); });

  auto overlay = Container::Tab({commit_pane, confirm_pane, help_pane}, &overlay_index_);

  // --------------------------------------------------------------- main view
  auto main_view = Renderer([this] {
    Tick();
    if (Animating()) {
      animation::RequestAnimationFrame();
    }

    Element view = vbox({
                       ui::Header(snapshot_),
                       ui::SummaryRow(snapshot_, bars_),
                       ui::FileList(snapshot_, selected_) | flex,
                       ui::Footer(message_, message_is_error_, ToastFade()),
                   }) |
                   bgcolor(ui::theme().bg);

    // Dimming the dashboard behind an overlay is the terminal's version of a
    // scrim: it stops the panels from competing with the dialog on top.
    if (overlay_open_) {
      view = view | dim;
    }
    return view;
  });

  auto root = main_view | Modal(overlay, &overlay_open_);

  // This handler is attached outside the modal wrapper on purpose.
  // Container::Stacked delivers events to its focused child, and the overlay
  // holds the only focusable component in the tree (the commit Input), so a
  // handler attached to main_view stops receiving keys the moment a modal is
  // wrapped around it. Returning false while an overlay is open lets the event
  // fall through to that overlay.
  root |= CatchEvent([this, &screen](const Event& event) {
    // Overlay routing lives here rather than on the panes themselves.
    // Container::Tab drops events unless it is focused, and the confirm and
    // help panes are plain Renderers with nothing focusable inside them, so
    // handlers attached to those panes never ran at all.
    if (overlay_open_) {
      switch (overlay_index_) {
        case kHelp:
          if (event.is_character() || event == Event::Escape || event == Event::Return) {
            CloseOverlay();
            return true;
          }
          return false;

        case kConfirm:
          if (event == Event::Character('y') || event == Event::Character('Y')) {
            PerformDiscard();
            return true;
          }
          if (event == Event::Character('n') || event == Event::Character('N') ||
              event == Event::Escape) {
            CloseOverlay();
            return true;
          }
          // Swallow any other typed key so a stray keystroke cannot answer a
          // destructive question by accident.
          return event.is_character();

        case kCommit:
        default:
          if (event == Event::Escape) {
            CloseOverlay();
            return true;
          }
          return false;  // the Input takes the rest
      }
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
