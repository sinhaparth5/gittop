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

#include "remote/provider.hpp"
#include "remote/token.hpp"
#include "ui/history_panel.hpp"
#include "ui/panels.hpp"
#include "ui/remote_panel.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

namespace gittop {

using namespace ftxui;  // NOLINT: the component DSL reads badly when qualified.

App::App(git::Repository repo, config::Config config, std::string config_path)
    : repo_(std::move(repo)),
      config_(std::move(config)),
      config_path_(std::move(config_path)) {}

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

  // Advanced on wall time rather than per frame, so the spinner turns at the
  // same speed whatever the frame rate happens to be. Ten frames at 90ms is a
  // full turn just under a second, which reads as working rather than frantic.
  constexpr float kSpinnerStep = 0.09F;
  spinner_accum_ += dt;
  while (spinner_accum_ >= kSpinnerStep) {
    spinner_accum_ -= kSpinnerStep;
    ++spinner_;
  }
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
  // A fetch in flight keeps the spinner turning. It stops the moment the
  // request lands, so this is bounded by the HTTP timeout rather than open.
  if (fetcher_.Running()) {
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

int& App::ActiveSelection() {
  switch (view_) {
    case ui::View::History:
      return commit_selected_;
    case ui::View::Branches:
      return branch_selected_;
    case ui::View::Graph:
    case ui::View::Remote:
    case ui::View::Status:
      break;
  }
  return selected_;
}

int App::ActiveCount() const {
  switch (view_) {
    case ui::View::History:
      return static_cast<int>(history_.commits.size());
    case ui::View::Branches:
      return static_cast<int>(history_.branches.size());
    case ui::View::Graph:
      return 0;  // the graph pans instead of selecting
    case ui::View::Remote:
      return 0;  // one repository, nothing to move between
    case ui::View::Status:
      break;
  }
  return static_cast<int>(snapshot_.entries.size());
}

int App::MaxGraphOffset() const {
  const int size = ui::GraphSeriesSize(history_, graph_.bucket);
  const int window = std::min(ui::GraphWindow(graph_.bucket), size);
  return std::max(0, size - window);
}

void App::PanGraph(int buckets) {
  graph_.offset = std::clamp(graph_.offset + buckets, 0, MaxGraphOffset());
}

void App::SetBucket(ui::Bucket bucket) {
  graph_.bucket = bucket;
  // Landing on "now" after a zoom is the predictable place to be; keeping a
  // raw offset would drop you somewhere unrelated at the new granularity.
  graph_.offset = 0;
  Note("bucket: " + ui::BucketName(bucket), false);
}

void App::DiscoverRemotes() {
  if (remotes_discovered_) {
    return;
  }
  remotes_discovered_ = true;

  for (const auto& [name, url] : repo_.ReadRemotes()) {
    model::RemoteRef ref = remote::ParseRemote(name, url);
    remote::ApplyHostOverrides(ref, config_);
    remotes_.push_back(std::move(ref));
  }
  remote_.ref = remote::ChooseRemote(remotes_);
}

void App::StartFetch() {
  DiscoverRemotes();
  if (!remote_.ref.valid() || fetcher_.Running()) {
    return;
  }

  const remote::Token token = remote::ResolveToken(remote_.ref, config_, config_path_);
  // Recorded now so the identity block can say "authenticated" while the
  // request is still in flight, rather than only once it comes back.
  remote_.token_source = token.source;
  remote_.token_origin = token.origin;
  remote_.state = model::FetchState::Loading;
  remote_.error.clear();
  remote_.hint.clear();

  fetcher_.Start(remote_.ref, token);
}

void App::EnsureRemote() {
  DiscoverRemotes();
  if (remote_.state == model::FetchState::Idle) {
    StartFetch();
  }
}

void App::CollectFetch() {
  model::RemoteSnapshot result;
  if (!fetcher_.Consume(&result)) {
    return;
  }
  // A cancelled fetch is the shutdown path; there is nothing to show and
  // nothing to complain about.
  if (result.error == "cancelled") {
    return;
  }

  remote_ = std::move(result);
  if (remote_.state == model::FetchState::Failed) {
    Note(remote_.error, true);
  } else if (view_ != ui::View::Remote) {
    // Only worth a toast when it landed somewhere the user is not looking.
    Note("remote: " + remote_.info.full_name, false);
  }
}

void App::Move(int delta) {
  const int count = ActiveCount();
  if (count == 0) {
    return;
  }
  int& selection = ActiveSelection();
  selection = std::clamp(selection + delta, 0, count - 1);
}

void App::SelectFirst() {
  ActiveSelection() = 0;
}

void App::SelectLast() {
  ActiveSelection() = std::max(0, ActiveCount() - 1);
}

void App::EnsureHistory() {
  if (history_loaded_) {
    return;
  }
  // 400 rows is more log than anyone scrolls in a session, and the activity
  // window is thirteen whole weeks so the heatmap grid comes out square.
  constexpr std::size_t kMaxLogCommits = 400;
  constexpr int kActivityDays = 91;

  history_ = repo_.ReadHistory(kMaxLogCommits, kActivityDays);
  history_loaded_ = true;

  commit_selected_ =
      std::clamp(commit_selected_, 0, std::max(0, static_cast<int>(history_.commits.size()) - 1));
  branch_selected_ =
      std::clamp(branch_selected_, 0, std::max(0, static_cast<int>(history_.branches.size()) - 1));
}

void App::SetView(ui::View view) {
  view_ = view;
  if (view == ui::View::Remote) {
    EnsureRemote();
    return;
  }
  if (view != ui::View::Status) {
    EnsureHistory();
  }
}

void App::Reload() {
  // On the remote view `r` means the network, everywhere else it means the
  // repository. Refreshing whichever one is not on screen would be a surprise.
  if (view_ == ui::View::Remote) {
    if (fetcher_.Running()) {
      Note("already fetching", false);
      return;
    }
    if (!remote_.ref.valid()) {
      Note("no remote to fetch", true);
      return;
    }
    StartFetch();
    Note("fetching " + remote_.ref.full_name(), false);
    return;
  }

  Refresh();
  history_loaded_ = false;
  if (view_ != ui::View::Status) {
    EnsureHistory();
  }
  Note("re-read the repository", false);
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
    // The log just gained a row, so the cached walk is stale.
    history_loaded_ = false;
    commit_selected_ = 0;
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

// Posted from the fetch worker to wake the event loop. A named special event
// rather than a keystroke, so nothing in the key routing can collide with it.
const Event kRemoteReady = Event::Special("gittop:remote-ready");

int App::Run() {
  Refresh();

  auto screen = ScreenInteractive::Fullscreen();

  fetcher_.SetNotifier([&screen] { screen.PostEvent(kRemoteReady); });

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
  auto main_view = Renderer([this, &screen] {
    Tick();
    if (Animating()) {
      animation::RequestAnimationFrame();
    }

    // Reading the live terminal size here is what makes the layout responsive:
    // the dom has no way to ask, but the screen does.
    const int width = screen.dimx();
    const int height = screen.dimy();

    Elements body{ui::Header(snapshot_), ui::TabBar(view_)};

    switch (view_) {
      case ui::View::Status:
        body.push_back(ui::SummaryRow(snapshot_, bars_, width < 84));
        body.push_back(ui::FileList(snapshot_, selected_) | flex);
        break;

      case ui::View::History:
        // The heatmap costs ten rows. On a short terminal the log is worth
        // more than the graph, so it goes first and the heatmap steps aside.
        if (height >= 30) {
          body.push_back(window(text(" ACTIVITY ") | bold | color(ui::theme().text_dim),
                                ui::ActivityPanel(history_)) |
                         color(ui::theme().border) | bgcolor(ui::theme().surface));
        }
        body.push_back(window(text(" COMMITS ") | bold | color(ui::theme().text_dim),
                              ui::CommitList(history_, commit_selected_)) |
                       color(ui::theme().border) | bgcolor(ui::theme().surface) | flex);
        break;

      case ui::View::Branches:
        body.push_back(window(text(" BRANCHES ") | bold | color(ui::theme().text_dim),
                              ui::BranchList(history_, branch_selected_)) |
                       color(ui::theme().border) | bgcolor(ui::theme().surface) | flex);
        break;

      case ui::View::Graph: {
        // The canvas cannot flex, so its row count is worked out from what is
        // left over. Chrome is header 1 + tabs 1 + footer 2, and the chart's
        // own frame is 6 (two borders, x axis, spacer, scrollbar, range line).
        const bool show_insights = height >= 30;
        const int insights_rows = show_insights ? 9 : 0;
        const int chart_rows = std::clamp(height - 10 - insights_rows, 4, 40);

        body.push_back(ui::GraphPanel(history_, graph_, width, chart_rows));
        if (show_insights) {
          body.push_back(ui::InsightsRow(history_, width));
        }
        break;
      }

      case ui::View::Remote:
        body.push_back(ui::RemotePanel(remote_, width, height, spinner_));
        break;
    }

    body.push_back(ui::Footer(message_, message_is_error_, ToastFade(), view_));

    Element view = vbox(std::move(body)) | bgcolor(ui::theme().bg);

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
    // The worker thread posts this after a fetch finishes. It carries no data
    // itself — it only wakes the loop so the result can be picked up here, on
    // the UI thread, where every other piece of state is touched.
    if (event == kRemoteReady) {
      CollectFetch();
      return true;
    }

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
    // The graph pans along a timeline rather than selecting rows, so it claims
    // the horizontal keys before the shared list movement below.
    if (view_ == ui::View::Graph) {
      const int step = std::max(1, ui::GraphWindow(graph_.bucket) / 8);
      if (event == Event::Character('h') || event == Event::ArrowLeft) {
        PanGraph(step);
        return true;
      }
      if (event == Event::Character('l') || event == Event::ArrowRight) {
        PanGraph(-step);
        return true;
      }
      if (event == Event::Character('d')) {
        SetBucket(ui::Bucket::Day);
        return true;
      }
      if (event == Event::Character('w')) {
        SetBucket(ui::Bucket::Week);
        return true;
      }
      if (event == Event::Character('m')) {
        SetBucket(ui::Bucket::Month);
        return true;
      }
      if (event == Event::Character('g')) {
        graph_.offset = MaxGraphOffset();
        return true;
      }
      if (event == Event::Character('G')) {
        graph_.offset = 0;
        return true;
      }
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
    if (event == Event::Character('1')) {
      SetView(ui::View::Status);
      return true;
    }
    if (event == Event::Character('2')) {
      SetView(ui::View::History);
      return true;
    }
    if (event == Event::Character('3')) {
      SetView(ui::View::Branches);
      return true;
    }
    if (event == Event::Character('4')) {
      SetView(ui::View::Graph);
      return true;
    }
    if (event == Event::Character('5')) {
      SetView(ui::View::Remote);
      return true;
    }
    if (event == Event::Tab) {
      switch (view_) {
        case ui::View::Status:
          SetView(ui::View::History);
          break;
        case ui::View::History:
          SetView(ui::View::Branches);
          break;
        case ui::View::Branches:
          SetView(ui::View::Graph);
          break;
        case ui::View::Graph:
          SetView(ui::View::Remote);
          break;
        case ui::View::Remote:
          SetView(ui::View::Status);
          break;
      }
      return true;
    }
    if (event == Event::Character('r')) {
      Reload();
      return true;
    }

    // Everything below acts on the working tree, so it only applies where the
    // working tree is on screen. Pressing `d` while reading the log should not
    // quietly discard whatever the status view happened to have selected.
    if (view_ == ui::View::Status) {
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
    }
    if (event == Event::Character('?')) {
      OpenOverlay(kHelp);
      return true;
    }
    return false;
  });

  screen.Loop(root);

  // Before `screen` goes out of scope, because the notifier captured it by
  // reference. A worker still in a ten-second timeout would otherwise post an
  // event into a destroyed screen on its way out.
  fetcher_.Shutdown();
  return 0;
}

}  // namespace gittop
