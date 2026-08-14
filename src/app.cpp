#include "app.hpp"

#include <ftxui/component/animation.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <system_error>
#include <thread>
#include <utility>

#include "remote/api.hpp"
#include "remote/client.hpp"
#include "remote/http.hpp"
#include "remote/oauth.hpp"
#include "remote/pipelines.hpp"
#include "remote/provider.hpp"
#include "remote/pulls.hpp"
#include "remote/token.hpp"
#include "ui/glyphs.hpp"
#include "ui/history_panel.hpp"
#include "ui/panels.hpp"
#include "ui/pipeline_panel.hpp"
#include "ui/pull_panel.hpp"
#include "ui/remote_panel.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

namespace gittop {

using namespace ftxui;  // NOLINT: the component DSL reads badly when qualified.

namespace {

// One page. More runs than anyone scrolls in a sitting, and one request rather
// than the pagination that a second page would need.
constexpr int kPipelineLimit = 30;

// Comes from CMake, which is where the version is actually decided. A literal
// here would be a third copy to forget: the splash, the settings page and
// `--version` would each be able to claim a different release.
#ifndef GITTOP_VERSION
#error "GITTOP_VERSION is not defined — build through CMake, which sets it from project(VERSION)"
#endif
constexpr const char* kVersion = GITTOP_VERSION;

constexpr int kPullLimit = 30;

// Fast enough that a pipeline finishing feels live, slow enough that an
// authenticated GitHub token spends 180 of its 5000 hourly requests watching
// one view for an hour.
constexpr int kDefaultRefreshSeconds = 20;
constexpr int kMinRefreshSeconds = 10;
constexpr int kMaxRefreshSeconds = 3600;

// Below this share of the budget the poll loop stops on its own. Whatever the
// user came to the terminal to do, spending the last of their rate limit on a
// view they left open is not it.
constexpr int kBudgetFloorPercent = 20;

// A wheel notch moves three rows, which is what every terminal list does and
// what a hand expects. One row per notch makes a long log feel stuck.
constexpr int kWheelRows = 3;

// More diff than any terminal is going to be scrolled through, and enough that
// a vendored dependency landing in one commit does not make the view unusable.
constexpr std::size_t kMaxDiffLines = 20000;

// Case-insensitive substring, ASCII only. A filter is a thing typed in a hurry
// to find a filename, not a search engine: no regex, no fuzzy matching, and no
// surprise about why `Makefile` did not match `makefile`.
bool Matches(const std::string& needle, const std::string& haystack) {
  if (needle.empty()) {
    return true;
  }
  if (needle.size() > haystack.size()) {
    return false;
  }
  const auto fold = [](char ch) {
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
  };
  for (std::size_t start = 0; start + needle.size() <= haystack.size(); ++start) {
    std::size_t i = 0;
    while (i < needle.size() && fold(needle[i]) == fold(haystack[start + i])) {
      ++i;
    }
    if (i == needle.size()) {
      return true;
    }
  }
  return false;
}

// Username halves for a token-over-https push. The token is the password in
// every case; only the name in front of it differs, and both providers ignore
// anything sensible in that slot.
const char* UsernameFor(model::Provider provider) {
  switch (provider) {
    case model::Provider::GitHub:
      return "x-access-token";
    case model::Provider::GitLab:
      return "oauth2";
    case model::Provider::Unknown:
      break;
  }
  return "git";
}

}  // namespace

App::App(git::Repository repo, config::Config config, std::string config_path)
    : repo_(std::move(repo)),
      config_(std::move(config)),
      config_path_(std::move(config_path)),
      transfer_sink_(std::make_shared<git::ProgressSink>()) {
  // Asked once here rather than per frame. The settings page reports whether
  // there is a file behind the path it prints, and that only changes when
  // gittop itself writes one.
  std::error_code ec;
  config_on_disk_ = std::filesystem::exists(config_path_, ec);
  ApplyConfig();
}

void App::ApplyConfig() {
  // A config with three mistakes in it should not report only the third. The
  // toast holds one line, so the first problem is the one shown and the rest
  // are counted — enough to know to keep reading.
  int problems = 0;
  std::string first_problem;
  const auto complain = [&problems, &first_problem](std::string what) {
    ++problems;
    if (first_problem.empty()) {
      first_problem = std::move(what);
    }
  };

  // Depth first: a palette is chosen in 24-bit and quantized on the way to the
  // screen, so the theme does not need to know what the terminal can show.
  ui::ColorDepth depth = ui::DetectColorDepth();
  const std::string wanted_depth = config_.Get("theme.depth", "auto");
  if (!ui::ParseColorDepth(wanted_depth, &depth)) {
    complain("unknown theme.depth '" + wanted_depth + "' — detecting instead");
    depth = ui::DetectColorDepth();
  }
  ui::SetColorDepth(depth);

  const std::string name = config_.Get("theme.name");
  if (!name.empty() && !ui::SetTheme(name)) {
    complain("unknown theme '" + name + "'");
  }

  // Glyphs are the second half of the same question the depth setting asks —
  // what this terminal can actually draw — so they are resolved in the same
  // place and before anything is rendered in a set the user replaced.
  ui::GlyphMode icons = ui::DetectGlyphMode();
  const std::string wanted_icons = config_.Get("theme.icons", "auto");
  if (!ui::ParseGlyphMode(wanted_icons, &icons)) {
    complain("unknown theme.icons '" + wanted_icons + "' — detecting instead");
    icons = ui::DetectGlyphMode();
  }
  ui::SetGlyphMode(icons);

  ui::PanelBorder border = ui::PanelBorder::Rounded;
  const std::string wanted_border = config_.Get("theme.border", "rounded");
  if (!ui::ParsePanelBorder(wanted_border, &border)) {
    complain("unknown theme.border '" + wanted_border + "'");
    border = ui::PanelBorder::Rounded;
  }
  ui::SetPanelBorder(border);

  // One flag for every moving thing: the eased bars, the spinners, the toast
  // fade, the skeleton shimmer and the splash. Off makes each of them snap to
  // its final state rather than disappear — a reduced-motion setting that also
  // removes information is a worse setting than none.
  ui::SetReducedMotion(!config_.GetBool("theme.animations", true));
  splash_ = config_.GetBool("theme.splash", true);

  // A user theme is the built-in one with roles replaced, which is why there is
  // no separate file format for it: the palette a config edits is the same
  // object a built-in palette is.
  constexpr const char* kColorPrefix = "theme.colors.";
  for (const auto& [key, value] : config_.values()) {
    if (key.rfind(kColorPrefix, 0) != 0) {
      continue;
    }
    const std::string role = key.substr(std::string(kColorPrefix).size());
    ui::Rgb rgb;
    if (!ui::ParseHexColor(value, &rgb)) {
      complain("theme.colors." + role + " is not a #rrggbb colour");
      continue;
    }
    if (!ui::OverrideColor(role, rgb)) {
      complain("theme.colors." + role + " is not a colour gittop has");
    }
  }

  // Which tabs exist and in what order. Applied before anything reads AllViews,
  // which is why it is here rather than at the first render: the digit keys are
  // positional, so a reordered list changes what `3` reaches.
  const std::string wanted_views = config_.Get("layout.views");
  if (!wanted_views.empty()) {
    std::vector<ui::View> views;
    std::string word;
    // Split on the same characters the keymap accepts, so a config can write
    // the list either way round without learning a second rule.
    const auto flush = [&views, &word, &complain] {
      if (word.empty()) {
        return;
      }
      ui::View view = ui::View::Status;
      if (ui::ParseViewName(word, &view)) {
        views.push_back(view);
      } else {
        complain("layout.views: '" + word + "' is not a view gittop has");
      }
      word.clear();
    };
    for (const char ch : wanted_views) {
      if (ch == ' ' || ch == '\t' || ch == ',') {
        flush();
        continue;
      }
      word.push_back(ch);
    }
    flush();

    if (!ui::SetViews(views)) {
      complain("layout.views named no views gittop has — keeping the defaults");
    }
  }

  compact_ = config_.GetBool("layout.compact", false);

  const std::string start = config_.Get("layout.start_view");
  if (!start.empty()) {
    ui::View view = ui::View::Status;
    if (!ui::ParseViewName(start, &view)) {
      complain("unknown layout.start_view '" + start + "'");
    } else {
      const std::vector<ui::View>& views = ui::AllViews();
      // A start view that is not in the tab bar would open on a screen no key
      // can get back to, which is worse than ignoring the line.
      if (std::find(views.begin(), views.end(), view) == views.end()) {
        complain("layout.start_view '" + start + "' is not one of layout.views");
      } else {
        view_ = view;
      }
    }
  }

  constexpr const char* kKeyPrefix = "keys.";
  for (const auto& [key, value] : config_.values()) {
    if (key.rfind(kKeyPrefix, 0) != 0) {
      continue;
    }
    std::string error;
    if (!keys_.Rebind(key.substr(std::string(kKeyPrefix).size()), value, &error)) {
      complain(error);
    }
  }

  // Said once at startup rather than left for the user to discover by pressing
  // a key and getting something else.
  for (const std::string& conflict : keys_.Conflicts()) {
    complain(conflict);
  }

  if (problems > 0) {
    Note(problems == 1 ? first_problem
                       : first_problem + "  (and " + std::to_string(problems - 1) + " more)",
         true);
  }
}

void App::Refresh() {
  snapshot_ = repo_.ReadStatus();
  // Read alongside the status because they answer one question together: a pile
  // of conflicted files means something quite different mid-rebase, and the
  // banner that says so has to appear in the same frame as the files do.
  operation_ = repo_.ReadOperation();

  // The working tree just changed under whatever diff was cached, and a diff of
  // a commit is the one kind that cannot go stale this way.
  if (diff_request_.source != model::DiffSource::Commit) {
    diff_loaded_ = false;
  }

  RebuildFilter();
  const int count = static_cast<int>(VisibleStatus().entries.size());
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
  const float k = ui::ReducedMotion() ? 1.0F : 1.0F - std::exp(-dt / kTau);

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

  // The overlay's own arrival. Tracked here rather than in the panes because
  // there are seven of them and they would all have to be told the same thing.
  if (!overlay_open_) {
    overlay_since_ = std::chrono::steady_clock::time_point{};
    ui::SetOverlayReveal(1.0F);
  } else {
    if (overlay_since_.time_since_epoch().count() == 0) {
      overlay_since_ = now;
    }
    // Short: a dialog you have to wait for is a dialog in the way. This is long
    // enough to read as the box arriving and too short to be a delay.
    constexpr float kOverlayRise = 0.12F;
    const float open_for = std::chrono::duration<float>(now - overlay_since_).count();
    ui::SetOverlayReveal(ui::ReducedMotion() ? 1.0F : open_for / kOverlayRise);
  }
}

// How far up the splash is, 1 while it holds and running back to 0 as it goes.
// Zero means it is not on screen at all, which is how the renderer decides
// whether to draw it — one number rather than a flag and a number that can
// disagree.
float App::SplashReveal() const {
  if (splash_until_.time_since_epoch().count() == 0) {
    return 0.0F;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now >= splash_until_) {
    return 0.0F;
  }
  if (ui::ReducedMotion()) {
    return 1.0F;
  }

  constexpr float kRise = 0.25F;
  constexpr float kFall = 0.35F;
  const float left = std::chrono::duration<float>(splash_until_ - now).count();
  const float shown = kSplashSeconds - left;
  if (shown < kRise) {
    return std::clamp(shown / kRise, 0.0F, 1.0F);
  }
  if (left < kFall) {
    return std::clamp(left / kFall, 0.0F, 1.0F);
  }
  return 1.0F;
}

// Any key gets past it, and the dashboard is already behind it — the status read
// happens in the constructor — so this is a card being dismissed rather than a
// loading screen being skipped.
bool App::DismissSplash() {
  if (SplashReveal() <= 0.0F) {
    return false;
  }
  splash_until_ = std::chrono::steady_clock::time_point{};
  return true;
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

// Without a cap the animation loop repaints as fast as the machine allows, which
// on an idle terminal easing one bar is a fan spinning up to do nothing. Thirty
// a second is the rate the 80ms time constant was tuned against, and it is only
// ever applied to a frame the animation asked for — an input-driven repaint is
// never delayed, because a keystroke that waits 33ms for a progress bar is a
// keystroke that feels broken.
void App::ThrottleFrame() {
  if (!animation_pending_) {
    return;
  }
  animation_pending_ = false;

  constexpr auto kBudget = std::chrono::duration<float>(1.0F / 30.0F);
  const auto now = std::chrono::steady_clock::now();
  const auto since = std::chrono::duration<float>(now - last_frame_);
  if (since < kBudget) {
    std::this_thread::sleep_for(kBudget - since);
  }
}

bool App::Animating() const {
  // Nothing eases, spins or fades when the user has asked for none of it, so
  // there is no reason to ask for a frame either.
  if (ui::ReducedMotion()) {
    return splash_until_.time_since_epoch().count() != 0 &&
           std::chrono::steady_clock::now() < splash_until_;
  }
  if (splash_until_.time_since_epoch().count() != 0 &&
      std::chrono::steady_clock::now() < splash_until_) {
    return true;
  }
  const ui::StatBars target = TargetBars();
  const auto moving = [](float a, float b) { return std::abs(a - b) > 0.0005F; };
  if (moving(bars_.staged, target.staged) || moving(bars_.unstaged, target.unstaged) ||
      moving(bars_.untracked, target.untracked) ||
      moving(bars_.conflicted, target.conflicted)) {
    return true;
  }
  // A fetch in flight keeps the spinner turning. It stops the moment the
  // request lands, so this is bounded by the HTTP timeout rather than open.
  if (fetcher_.Running() || pipeline_fetcher_.Running() || job_fetcher_.Running() ||
      pull_fetcher_.Running()) {
    return true;
  }
  // A transfer's progress bar is the one thing here that has to repaint while
  // nothing else is happening, because the numbers behind it change on a worker
  // thread that posts no events.
  if (transfer_fetcher_.Running()) {
    return true;
  }
  // An overlay that has not finished arriving. Bounded by 120ms, and by the
  // reveal being pinned at 1 when motion is off.
  if (overlay_open_ && ui::OverlayReveal() < 1.0F) {
    return true;
  }
  // A sign-in waiting for approval. Neither of the two things moving on that
  // pane is driven by an event: the spinner needs frames, and the expiry
  // countdown would otherwise sit at whatever it read when the code arrived —
  // a clock that has stopped is worse than no clock. Bounded by the code
  // expiring and by the overlay being open at all.
  if (overlay_open_ && overlay_index_ == kSignIn &&
      (signin_stage_ == ui::SignInStage::Waiting ||
       signin_stage_ == ui::SignInStage::Starting)) {
    return true;
  }
  // A live pipeline has a spinner per row. Bounded by the run finishing and by
  // the view being open — leaving the CI view stops it, and the ticker's one
  // frame a second is not enough to animate anything.
  if (view_ == ui::View::Pipelines && pipelines_.running > 0) {
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
    case ui::View::Pipelines:
      return pipeline_selected_;
    case ui::View::Pulls:
      return pull_selected_;
    case ui::View::Diff:
      return diff_line_;
    case ui::View::Stashes:
      return stash_selected_;
    case ui::View::Settings:
      return settings_selected_;
    case ui::View::Graph:
    case ui::View::Remote:
    case ui::View::Status:
      break;
  }
  return selected_;
}

int App::ActiveCount() const {
  // Every one of these counts what is on screen rather than what was read, so
  // the cursor can never point past the end of a filtered list.
  switch (view_) {
    case ui::View::History:
      return static_cast<int>(VisibleHistory().commits.size());
    case ui::View::Branches:
      return static_cast<int>(VisibleHistory().branches.size());
    case ui::View::Pipelines:
      return static_cast<int>(VisiblePipelines().runs.size());
    case ui::View::Pulls:
      return static_cast<int>(VisiblePulls().pulls.size());
    case ui::View::Diff:
      return static_cast<int>(diff_.lines.size());
    case ui::View::Stashes:
      return static_cast<int>(VisibleStashes().entries.size());
    // Built rather than stored, so the count and what the panel draws come out
    // of the same function and cannot disagree about how many rows there are.
    case ui::View::Settings:
      return ui::SettingsRowCount(SettingsViewState());
    case ui::View::Graph:
      return 0;  // the graph pans instead of selecting
    case ui::View::Remote:
      return 0;  // one repository, nothing to move between
    case ui::View::Status:
      break;
  }
  return static_cast<int>(VisibleStatus().entries.size());
}

const model::StatusSnapshot& App::VisibleStatus() const {
  return filter_.empty() ? snapshot_ : filtered_status_;
}
const model::HistorySnapshot& App::VisibleHistory() const {
  return filter_.empty() ? history_ : filtered_history_;
}
const model::PipelineSnapshot& App::VisiblePipelines() const {
  return filter_.empty() ? pipelines_ : filtered_pipelines_;
}
const model::PullSnapshot& App::VisiblePulls() const {
  return filter_.empty() ? pulls_ : filtered_pulls_;
}
const model::StashList& App::VisibleStashes() const {
  return filter_.empty() ? stashes_ : filtered_stashes_;
}

void App::RebuildFilter() {
  // Nothing to mirror. Leaving the copies alone rather than clearing them keeps
  // this the cheap path, which matters because it runs on every keystroke of
  // the filter box and on every refresh whether or not one is active.
  if (filter_.empty()) {
    return;
  }

  filtered_status_ = snapshot_;
  filtered_status_.entries.clear();
  filtered_status_.staged = 0;
  filtered_status_.unstaged = 0;
  filtered_status_.untracked = 0;
  filtered_status_.conflicted = 0;
  for (const model::StatusEntry& entry : snapshot_.entries) {
    if (!Matches(filter_, entry.path) && !Matches(filter_, entry.old_path)) {
      continue;
    }
    // The counts drive the summary bars, so they describe what is shown. A bar
    // reading forty while four rows are listed is the wrong kind of honest.
    if (entry.stage == model::Stage::Conflict) {
      ++filtered_status_.conflicted;
    } else if (entry.stage == model::Stage::Index) {
      ++filtered_status_.staged;
    } else if (entry.change == model::Change::Untracked) {
      ++filtered_status_.untracked;
    } else {
      ++filtered_status_.unstaged;
    }
    filtered_status_.entries.push_back(entry);
  }

  filtered_history_ = history_;
  filtered_history_.commits.clear();
  filtered_history_.branches.clear();
  for (const model::Commit& commit : history_.commits) {
    if (!Matches(filter_, commit.summary) && !Matches(filter_, commit.author) &&
        !Matches(filter_, commit.short_id)) {
      continue;
    }
    model::Commit copy = commit;
    // The lane gutter describes the shape of the whole history, and hiding the
    // rows between two commits makes it draw connections that are no longer on
    // screen. Dropping it is the honest answer; a filtered log is a list.
    copy.row.clear();
    filtered_history_.commits.push_back(std::move(copy));
  }
  for (const model::Branch& branch : history_.branches) {
    if (Matches(filter_, branch.name) || Matches(filter_, branch.upstream)) {
      filtered_history_.branches.push_back(branch);
    }
  }

  filtered_pipelines_ = pipelines_;
  filtered_pipelines_.runs.clear();
  for (const model::Pipeline& run : pipelines_.runs) {
    if (Matches(filter_, run.title) || Matches(filter_, run.branch) ||
        Matches(filter_, run.commit_title) || Matches(filter_, run.actor)) {
      filtered_pipelines_.runs.push_back(run);
    }
  }

  filtered_pulls_ = pulls_;
  filtered_pulls_.pulls.clear();
  for (const model::PullRequest& pull : pulls_.pulls) {
    if (Matches(filter_, pull.title) || Matches(filter_, pull.author) ||
        Matches(filter_, pull.source_branch) || Matches(filter_, pull.target_branch)) {
      filtered_pulls_.pulls.push_back(pull);
    }
  }

  filtered_stashes_.entries.clear();
  for (const model::Stash& stash : stashes_.entries) {
    if (Matches(filter_, stash.message) || Matches(filter_, stash.branch)) {
      filtered_stashes_.entries.push_back(stash);
    }
  }

  // A filter that narrowed the list out from under the cursor leaves it past
  // the end, and every subsequent Move would clamp against a stale count.
  int& selection = ActiveSelection();
  selection = std::clamp(selection, 0, std::max(0, ActiveCount() - 1));
}

int App::FilterMatches() const {
  return ActiveCount();
}

int App::FilterTotal() const {
  switch (view_) {
    case ui::View::History:
      return static_cast<int>(history_.commits.size());
    case ui::View::Branches:
      return static_cast<int>(history_.branches.size());
    case ui::View::Pipelines:
      return static_cast<int>(pipelines_.runs.size());
    case ui::View::Pulls:
      return static_cast<int>(pulls_.pulls.size());
    case ui::View::Stashes:
      return static_cast<int>(stashes_.entries.size());
    case ui::View::Diff:
    case ui::View::Graph:
    case ui::View::Remote:
    // Filterable in principle and pointless in practice: eleven rows with the
    // headings that group them, and hiding the headings is what a filter would
    // do. `/` says there is nothing to filter here rather than emptying it.
    case ui::View::Settings:
      return 0;
    case ui::View::Status:
      break;
  }
  return static_cast<int>(snapshot_.entries.size());
}

void App::OpenFilter() {
  if (FilterTotal() == 0) {
    Note("nothing to filter on this view", false);
    return;
  }
  OpenOverlay(kFilter);
}

void App::CloseFilter(bool keep) {
  if (!keep) {
    filter_.clear();
  }
  RebuildFilter();
  // Whether it was kept or cleared, the list under the cursor just changed
  // size, and the selection has to land somewhere that exists.
  int& selection = ActiveSelection();
  selection = std::clamp(selection, 0, std::max(0, ActiveCount() - 1));
  CloseOverlay();
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

  // ChooseRemote holds the preference order; this only needs to know which
  // index it landed on, because the user can now walk the list from there.
  const model::RemoteRef chosen = remote::ChooseRemote(remotes_);
  for (std::size_t i = 0; i < remotes_.size(); ++i) {
    if (remotes_[i].name == chosen.name) {
      remote_index_ = i;
      break;
    }
  }
  remote_.ref = chosen;

  // Where the token came from is settled once, here, rather than at the first
  // fetch: the CI view has to know whether it is authenticated before it
  // decides to poll, and that can happen before any request has been made. The
  // value itself is not kept — it is resolved again, per fetch, by the task
  // that needs it, so nothing long-lived in App holds a secret.
  const remote::Token token = remote::ResolveToken(remote_.ref, config_, config_path_, &session_token_);
  remote_.token_source = token.source;
  remote_.token_origin = token.origin;
}

void App::NextRemote() {
  DiscoverRemotes();
  if (remotes_.size() < 2) {
    Note(remotes_.empty() ? "this repository has no remote" : "only one remote", false);
    return;
  }
  remote_index_ = (remote_index_ + 1) % remotes_.size();

  // Every cached network answer describes the remote that is being left, so all
  // three go. Keeping them would show one remote's pull requests under
  // another's name until the next refresh happened to land.
  const model::RemoteRef ref = remotes_[remote_index_];
  remote_ = model::RemoteSnapshot{};
  remote_.ref = ref;
  pipelines_ = model::PipelineSnapshot{};
  pulls_ = model::PullSnapshot{};
  jobs_ = model::JobList{};
  jobs_open_ = false;
  pull_details_open_ = false;
  pipeline_selected_ = 0;
  pull_selected_ = 0;
  last_pipeline_fetch_ = {};

  const remote::Token token = remote::ResolveToken(remote_.ref, config_, config_path_, &session_token_);
  remote_.token_source = token.source;
  remote_.token_origin = token.origin;

  Note("remote: " + ref.name + " · " + (ref.url.empty() ? "no url" : ref.host), false);

  // Only the view actually on screen re-fetches. The other two stay Idle and
  // load on the same lazy terms as always.
  switch (view_) {
    case ui::View::Remote:
      EnsureRemote();
      break;
    case ui::View::Pipelines:
      EnsurePipelines();
      break;
    case ui::View::Pulls:
      EnsurePulls();
      break;
    // None of these describes the remote, so switching remotes leaves them
    // exactly as they were.
    case ui::View::Status:
    case ui::View::History:
    case ui::View::Branches:
    case ui::View::Graph:
    case ui::View::Diff:
    case ui::View::Stashes:
    // Reads the new remote out of App on the next frame, so there is nothing
    // to kick off here.
    case ui::View::Settings:
      break;
  }
}

void App::StartFetch() {
  DiscoverRemotes();
  if (!remote_.ref.valid() || fetcher_.Running()) {
    return;
  }

  const remote::Token token = remote::ResolveToken(remote_.ref, config_, config_path_, &session_token_);
  // Recorded now so the identity block can say "authenticated" while the
  // request is still in flight, rather than only once it comes back.
  remote_.token_source = token.source;
  remote_.token_origin = token.origin;
  remote_.state = model::FetchState::Loading;
  remote_.error.clear();
  remote_.hint.clear();

  // Everything the worker touches is copied into the task. It outlives this
  // call and must not reach back into App from another thread.
  const model::RemoteRef ref = remote_.ref;
  fetcher_.Start([ref, token](const std::atomic<bool>& cancel) {
    remote::HttpClient client;
    return remote::FetchRepoInfo(ref, token, client, &cancel);
  });
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

void App::StartPipelineFetch() {
  DiscoverRemotes();
  if (!remote_.ref.valid() || pipeline_fetcher_.Running()) {
    return;
  }

  // A detached or unborn HEAD has no branch to filter by, and ReadStatus
  // reports those as a parenthesised placeholder rather than a ref anything
  // would match. Asking for every branch beats asking for one that cannot
  // exist and rendering the empty answer as "no CI".
  const bool on_a_branch = !snapshot_.head_detached && !snapshot_.head_unborn &&
                           !snapshot_.branch.empty() && snapshot_.branch.front() != '(';
  const std::string branch = on_a_branch ? snapshot_.branch : std::string();

  pipelines_.state = model::FetchState::Loading;
  pipelines_.branch = branch;
  pipelines_.error.clear();
  pipelines_.hint.clear();
  last_pipeline_fetch_ = std::chrono::steady_clock::now();

  const remote::Token token = remote::ResolveToken(remote_.ref, config_, config_path_, &session_token_);
  const model::RemoteRef ref = remote_.ref;
  pipeline_fetcher_.Start([ref, token, branch](const std::atomic<bool>& cancel) {
    remote::HttpClient client;
    return remote::FetchPipelines(ref, token, branch, kPipelineLimit, client, &cancel);
  });
}

void App::EnsurePipelines() {
  DiscoverRemotes();
  if (pipelines_.state == model::FetchState::Idle) {
    StartPipelineFetch();
  }
}

void App::CollectPipelines() {
  model::PipelineSnapshot result;
  if (!pipeline_fetcher_.Consume(&result)) {
    return;
  }
  if (result.error == "cancelled") {
    return;
  }

  pipelines_ = std::move(result);
  // The mirror has to describe the fetch that just landed rather than the one
  // before it. Without this a filtered CI view keeps drawing the previous
  // response for the rest of the session, which is the one thing this view
  // exists not to do: every twenty seconds it silently redraws stale runs.
  RebuildFilter();
  pipeline_selected_ = std::clamp(
      pipeline_selected_, 0, std::max(0, static_cast<int>(VisiblePipelines().runs.size()) - 1));

  if (pipelines_.state == model::FetchState::Failed) {
    Note(pipelines_.error, true);
    return;
  }

  // A refresh that arrives under an open drill-down re-reads the jobs, but only
  // while the run is still going. A finished run's jobs will not change again,
  // and re-asking every twenty seconds would spend the budget the poll interval
  // was chosen to protect.
  //
  // Through the visible list, because pipeline_selected_ counts rows on screen.
  const model::PipelineSnapshot& visible = VisiblePipelines();
  if (jobs_open_ && pipeline_selected_ < static_cast<int>(visible.runs.size())) {
    const model::Pipeline& run = visible.runs[static_cast<std::size_t>(pipeline_selected_)];
    if (jobs_.pipeline_id != run.id || !model::RunFinished(run.status)) {
      StartJobFetch();
    }
  }
}

void App::StartJobFetch() {
  if (!remote_.ref.valid() || job_fetcher_.Running()) {
    return;
  }
  // The visible list, not the read one: pipeline_selected_ is an index into
  // what is on screen, so resolving it against the unfiltered runs opens a
  // different run's jobs under the selected row's number whenever `/` is on.
  const model::PipelineSnapshot& visible = VisiblePipelines();
  if (pipeline_selected_ < 0 || pipeline_selected_ >= static_cast<int>(visible.runs.size())) {
    return;
  }
  const model::Pipeline& run = visible.runs[static_cast<std::size_t>(pipeline_selected_)];
  if (run.id.empty()) {
    return;
  }

  // Rows already on screen for this same run stay there while the refresh is in
  // flight; a different run starts from nothing, so the pane cannot briefly
  // label one run's jobs with another's number.
  if (jobs_.pipeline_id != run.id) {
    jobs_ = model::JobList{};
    jobs_.pipeline_id = run.id;
  }
  jobs_.state = model::FetchState::Loading;

  const remote::Token token = remote::ResolveToken(remote_.ref, config_, config_path_, &session_token_);
  const model::RemoteRef ref = remote_.ref;
  const std::string id = run.id;
  job_fetcher_.Start([ref, token, id](const std::atomic<bool>& cancel) {
    remote::HttpClient client;
    return remote::FetchJobs(ref, token, id, client, &cancel);
  });
}

void App::CollectJobs() {
  model::JobList result;
  if (!job_fetcher_.Consume(&result)) {
    return;
  }
  if (result.error == "cancelled") {
    return;
  }
  jobs_ = std::move(result);
  if (jobs_.state == model::FetchState::Failed) {
    Note(jobs_.error, true);
  }
}

void App::ToggleJobs() {
  // What is on screen, so a filter that matches nothing answers "no runs to
  // open" rather than quietly drilling into a run the user cannot see.
  if (VisiblePipelines().runs.empty()) {
    Note("no runs to open", false);
    return;
  }
  jobs_open_ = !jobs_open_;
  if (jobs_open_) {
    StartJobFetch();
  }
}

void App::StartPullFetch() {
  DiscoverRemotes();
  if (!remote_.ref.valid() || pull_fetcher_.Running()) {
    return;
  }

  const bool on_a_branch = !snapshot_.head_detached && !snapshot_.head_unborn &&
                           !snapshot_.branch.empty() && snapshot_.branch.front() != '(';
  const std::string branch = on_a_branch ? snapshot_.branch : std::string();

  pulls_.state = model::FetchState::Loading;
  pulls_.branch = branch;
  pulls_.error.clear();
  pulls_.hint.clear();

  const remote::Token token = remote::ResolveToken(remote_.ref, config_, config_path_, &session_token_);
  const model::RemoteRef ref = remote_.ref;
  pull_fetcher_.Start([ref, token, branch](const std::atomic<bool>& cancel) {
    remote::HttpClient client;
    return remote::FetchPulls(ref, token, branch, kPullLimit, client, &cancel);
  });
}

void App::EnsurePulls() {
  DiscoverRemotes();
  if (pulls_.state == model::FetchState::Idle) {
    StartPullFetch();
  }
}

void App::CollectPulls() {
  model::PullSnapshot result;
  if (!pull_fetcher_.Consume(&result)) {
    return;
  }
  if (result.error == "cancelled") {
    return;
  }

  pulls_ = std::move(result);
  RebuildFilter();
  pull_selected_ =
      std::clamp(pull_selected_, 0, std::max(0, static_cast<int>(VisiblePulls().pulls.size()) - 1));
  if (pulls_.state == model::FetchState::Failed) {
    Note(pulls_.error, true);
  }
}

void App::ToggleDetails() {
  if (pulls_.pulls.empty()) {
    Note("nothing to open", false);
    return;
  }
  // Nothing to fetch: the detail pane is built entirely from the list response,
  // which is why the cursor may keep moving with it open while the CI view's
  // drill-down closes.
  pull_details_open_ = !pull_details_open_;
}

int App::RefreshInterval() const {
  const int seconds = config_.GetInt("pipelines.refresh_seconds", kDefaultRefreshSeconds);
  return std::clamp(seconds, kMinRefreshSeconds, kMaxRefreshSeconds);
}

bool App::AutoRefreshAllowed(std::string* reason) const {
  reason->clear();

  if (!config_.GetBool("pipelines.auto_refresh", true)) {
    *reason = "auto-refresh off in config";
    return false;
  }
  if (!remote_.ref.valid()) {
    *reason = "no remote";
    return false;
  }
  // Sixty requests an hour is GitHub's anonymous allowance, and a twenty-second
  // poll would spend it in twenty minutes on this view alone. Anonymous means
  // manual, and the panel says so rather than looking stuck.
  if (remote_.token_source == model::TokenSource::None) {
    *reason = "anonymous — refresh with " + keys_.KeyFor(ui::Action::Reload);
    return false;
  }

  const model::RateLimit& rate = pipelines_.rate;
  if (rate.known && rate.limit > 0 && rate.remaining >= 0 &&
      rate.remaining * 100 < rate.limit * kBudgetFloorPercent) {
    *reason = "API budget low — refresh with " + keys_.KeyFor(ui::Action::Reload);
    return false;
  }
  return true;
}

int App::SecondsToRefresh() const {
  if (last_pipeline_fetch_.time_since_epoch().count() == 0) {
    return -1;
  }
  const float age =
      std::chrono::duration<float>(std::chrono::steady_clock::now() - last_pipeline_fetch_)
          .count();
  return std::max(0, RefreshInterval() - static_cast<int>(age));
}

void App::MaybeAutoRefresh() {
  // Only where it can be seen. Polling a view nobody is looking at is how a
  // dashboard turns into a background job.
  if (view_ != ui::View::Pipelines || pipeline_fetcher_.Running()) {
    return;
  }
  if (pipelines_.state == model::FetchState::Idle) {
    return;  // the first load is EnsurePipelines' job, not the timer's
  }
  std::string reason;
  if (!AutoRefreshAllowed(&reason)) {
    return;
  }
  if (SecondsToRefresh() != 0) {
    return;
  }
  StartPipelineFetch();
}

ui::PipelineView App::PipelineViewState() const {
  ui::PipelineView view;
  view.selected = pipeline_selected_;
  view.jobs_open = jobs_open_;

  std::string reason;
  const bool allowed = AutoRefreshAllowed(&reason);
  view.auto_paused = !allowed;
  view.paused_reason = std::move(reason);
  view.next_refresh = allowed ? SecondsToRefresh() : -1;
  return view;
}

void App::StartTransfer(TransferKind kind) {
  DiscoverRemotes();
  if (transfer_fetcher_.Running()) {
    Note("a transfer is already running", false);
    return;
  }
  if (remotes_.empty()) {
    Note("this repository has no remote", true);
    return;
  }

  // The remote to talk to is the active one by name, not by parsed provider: a
  // remote gittop cannot classify still pushes and pulls perfectly well, and
  // refusing one because its host is not GitHub or GitLab would be absurd.
  const std::string remote_name = remotes_[remote_index_].name;
  const std::string remote_url = remotes_[remote_index_].url;

  // An encrypted ssh key has to be answered before the worker starts, not
  // during it: ssh asks its question from inside the transfer, at a point where
  // the UI thread is free but the worker is already blocked in libgit2 waiting
  // for that same answer. Asking first turns a deadlock into a dialog.
  if (ssh_passphrase_.empty() && git::NeedsPassphrase(remote_url)) {
    RequestPassphrase(kind);
    return;
  }

  // The token is resolved here and lives only inside the task, exactly as the
  // HTTP fetches do. It is never stored on App and never reaches ui/.
  const remote::Token token = remote::ResolveToken(remote_.ref, config_, config_path_, &session_token_);
  git::Credentials credentials;
  if (token.present()) {
    credentials.username = UsernameFor(remote_.ref.provider);
    credentials.password = token.value;
  }

  const std::string path = repo_.WorkdirPath();
  std::shared_ptr<git::ProgressSink> sink = transfer_sink_;
  sink->Publish(git::TransferProgress{});

  // The askpass channel belongs to exactly one transfer: opened here, closed in
  // CollectTransfer. InstallAskpassEnv writes to gittop's own environment,
  // because libgit2's exec transport offers no way to set the child's — which is
  // why it happens here on the UI thread, before any worker exists to race it.
  askpass_.reset();
  if (!ssh_passphrase_.empty() && git::IsSshUrl(remote_url)) {
    auto server = std::make_unique<git::AskpassServer>(ssh_passphrase_);
    if (server->ok() && git::InstallAskpassEnv(*server)) {
      askpass_ = std::move(server);
    } else {
      // Not worth refusing the transfer over. An agent or an unencrypted key
      // still authenticates, and the failure if neither does is the same one
      // that happened before any of this existed.
      git::ClearAskpassEnv();
    }
  }

  transfer_kind_ = kind;
  transfer_cancelling_ = false;
  OpenOverlay(kTransfer);

  switch (kind) {
    case TransferKind::Fetch:
      transfer_fetcher_.Start([path, remote_name, credentials, sink](
                                  const std::atomic<bool>& cancel) {
        return git::Fetch(path, remote_name, credentials, sink, &cancel);
      });
      break;
    case TransferKind::Pull:
      transfer_fetcher_.Start([path, remote_name, credentials, sink](
                                  const std::atomic<bool>& cancel) {
        return git::Pull(path, remote_name, credentials, sink, &cancel);
      });
      break;
    case TransferKind::Push:
      transfer_fetcher_.Start([path, remote_name, credentials, sink](
                                  const std::atomic<bool>& cancel) {
        return git::Push(path, remote_name, /*set_upstream=*/true, credentials, sink, &cancel);
      });
      break;
    case TransferKind::Prune:
      transfer_fetcher_.Start([path, remote_name, credentials, sink](
                                  const std::atomic<bool>& cancel) {
        return git::Prune(path, remote_name, credentials, sink, &cancel);
      });
      break;
    case TransferKind::None:
      break;
  }
}

void App::RequestPush() {
  DiscoverRemotes();
  if (remotes_.empty()) {
    Note("this repository has no remote", true);
    return;
  }
  // Push is the only thing in gittop that changes something other people can
  // see, which is the whole reason it asks first. Fetch and pull only ever
  // change this working copy, so neither does.
  confirm_kind_ = ConfirmKind::Push;
  OpenOverlay(kConfirm);
}

void App::PerformPush() {
  CloseOverlay();
  StartTransfer(TransferKind::Push);
}

void App::RequestPrune() {
  DiscoverRemotes();
  if (remotes_.empty()) {
    Note("this repository has no remote", true);
    return;
  }
  // Asks first for the same reason push does, from the other direction: this one
  // only changes the local copy, but it is the only thing in gittop that deletes
  // a ref rather than adding to one, and which refs go is decided by a server
  // rather than by anything on screen.
  confirm_kind_ = ConfirmKind::Prune;
  OpenOverlay(kConfirm);
}

void App::PerformPrune() {
  CloseOverlay();
  StartTransfer(TransferKind::Prune);
}

void App::RequestPassphrase(TransferKind kind) {
  pending_transfer_ = kind;
  passphrase_input_.clear();
  OpenOverlay(kPassphrase);
}

void App::SubmitPassphrase() {
  // Enter on an empty field is not an answer. Serving an empty passphrase would
  // burn one of ssh's three attempts to say nothing.
  if (passphrase_input_.empty()) {
    return;
  }
  ssh_passphrase_ = passphrase_input_;
  // The overlay's buffer is the only copy anything under ui/ ever sees, and it
  // does not outlive the question being answered.
  passphrase_input_.clear();
  passphrase_rejected_ = false;

  const TransferKind kind = pending_transfer_;
  pending_transfer_ = TransferKind::None;
  CloseOverlay();
  StartTransfer(kind);
}

void App::CancelPassphrase() {
  passphrase_input_.clear();
  pending_transfer_ = TransferKind::None;
  passphrase_rejected_ = false;
  CloseOverlay();
  Note("transfer cancelled", false);
}

// --------------------------------------------------------------------- sign in

void App::RequestSignIn() {
  DiscoverRemotes();
  if (remotes_.empty() || !remote_.ref.valid()) {
    Note("no GitHub or GitLab remote to sign in to", false);
    return;
  }

  // The environment is checked first by ResolveToken and cannot be overridden
  // from in here, so signing in while GITHUB_TOKEN is set would collect a token
  // that never gets used. Saying so beats a sign-in that appears to work.
  const remote::Token existing =
      remote::ResolveToken(remote_.ref, config_, config_path_, &session_token_);
  if (existing.source == model::TokenSource::Environment) {
    Note("already authenticated from " + existing.origin + "; unset it to sign in", false);
    return;
  }

  signin_ref_ = remote_.ref;
  signin_origin_ = remote::OAuthOrigin(signin_ref_, config_);
  signin_client_id_ = remote::ClientIdFor(signin_ref_, config_);
  signin_code_ = remote::DeviceCode{};
  signin_error_.clear();
  signin_hint_.clear();
  signin_saved_to_.clear();
  signin_save_error_.clear();
  signin_browser_failed_ = false;
  token_input_.clear();

  if (signin_client_id_.empty()) {
    // No application registered for this host, which is the normal case for a
    // self-hosted instance and for a build with no id compiled in. The guided
    // token is not a degraded mode, it is the other supported route.
    signin_stage_ = ui::SignInStage::Paste;
    OpenOverlay(kSignIn);
    return;
  }

  signin_stage_ = ui::SignInStage::Starting;
  OpenOverlay(kSignIn);
  StartDeviceFlow();
}

void App::StartDeviceFlow() {
  const model::RemoteRef ref = signin_ref_;
  const std::string origin = signin_origin_;
  const std::string client_id = signin_client_id_;
  device_fetcher_.Start([ref, origin, client_id](const std::atomic<bool>& cancel) {
    remote::HttpClient http;
    return remote::BeginDeviceFlow(ref, origin, client_id, http, &cancel);
  });
}

void App::CollectDeviceCode() {
  remote::DeviceCode result;
  if (!device_fetcher_.Consume(&result)) {
    return;
  }
  if (!overlay_open_ || overlay_index_ != kSignIn) {
    return;  // the overlay was closed while the request was in flight
  }

  if (!result.ok()) {
    // A host that cannot start a device flow can nearly always still issue a
    // personal access token, so this falls through to that rather than ending
    // the sign-in. The reason is kept and shown above the fallback.
    signin_error_ = result.error;
    signin_hint_ = result.hint;
    signin_stage_ = ui::SignInStage::Paste;
    return;
  }

  signin_code_ = std::move(result);
  signin_interval_ = signin_code_.interval_seconds;
  signin_stage_ = ui::SignInStage::Waiting;

  // Opened without being asked, because the whole point of the flow is that the
  // approval happens in a browser and every second spent copying a URL by hand
  // is a second off the code's expiry. It is not fatal when there is no opener:
  // the URL is on screen either way, and the pane says so.
  signin_browser_failed_ = !remote::OpenInBrowser(signin_code_.verification_uri_complete.empty()
                                                      ? signin_code_.verification_uri
                                                      : signin_code_.verification_uri_complete);
  SchedulePoll();
}

void App::SchedulePoll() {
  const model::RemoteRef ref = signin_ref_;
  const std::string origin = signin_origin_;
  const std::string client_id = signin_client_id_;
  const std::string device_code = signin_code_.device_code;
  const int interval = std::max(1, signin_interval_);

  poll_fetcher_.Start([ref, origin, client_id, device_code,
                       interval](const std::atomic<bool>& cancel) {
    // The wait happens here rather than on a timer so that esc reaches it: the
    // cancel flag is already threaded into this task, and a std::thread asleep
    // for five seconds is the cheapest possible scheduler for a flow that polls
    // twelve times.
    for (int slept = 0; slept < interval * 20; ++slept) {
      if (cancel.load()) {
        remote::PollResult cancelled;
        cancelled.state = remote::PollState::Failed;
        cancelled.error = "cancelled";
        return cancelled;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    remote::HttpClient http;
    return remote::PollDeviceFlow(ref, origin, client_id, device_code, http, &cancel);
  });
}

void App::CollectPoll() {
  remote::PollResult result;
  if (!poll_fetcher_.Consume(&result)) {
    return;
  }
  if (!overlay_open_ || overlay_index_ != kSignIn ||
      signin_stage_ != ui::SignInStage::Waiting) {
    return;
  }
  if (result.error == "cancelled") {
    return;
  }

  switch (result.state) {
    case remote::PollState::Granted:
      AdoptToken(std::move(result.token));
      return;

    case remote::PollState::SlowDown:
      // RFC 8628 says the interval increases and the client must respect it;
      // ignoring it is how a provider starts refusing the flow outright.
      signin_interval_ = result.interval_seconds > 0 ? result.interval_seconds
                                                     : signin_interval_ + 5;
      SchedulePoll();
      return;

    case remote::PollState::Pending:
      // Also where a single failed round trip lands. The code has not expired,
      // so there is nothing to report and nothing to do but ask again.
      SchedulePoll();
      return;

    case remote::PollState::Expired:
    case remote::PollState::Denied:
    case remote::PollState::Failed:
      break;
  }

  signin_stage_ = ui::SignInStage::Failed;
  signin_error_ = result.error;
  signin_hint_ = result.hint;
}

void App::SubmitPastedToken() {
  if (token_input_.empty()) {
    return;
  }
  // The overlay's buffer is the only copy ui/ ever holds and it does not
  // outlive the paste, exactly as the passphrase input does not.
  std::string token = token_input_;
  token_input_.clear();
  AdoptToken(std::move(token));
}

void App::AdoptToken(std::string token) {
  session_token_.host = signin_ref_.host;
  session_token_.value = std::move(token);

  // In memory first and on disk second, so a config that cannot be written
  // still leaves a working session rather than a sign-in that did nothing.
  config_.Set("hosts." + signin_ref_.host + ".token", session_token_.value);
  std::string error;
  if (config_.Save(config_path_, &error)) {
    signin_saved_to_ = config_path_;
  } else {
    signin_save_error_ = error;
  }

  signin_stage_ = ui::SignInStage::Granted;
  signin_code_ = remote::DeviceCode{};  // the device code has served its purpose

  // Every cached answer was fetched as somebody else — usually as nobody — so
  // none of them describes what this token can now see. A private repository
  // that 404'd anonymously is the whole reason for signing in, and leaving the
  // 404 on screen would make the sign-in look like it failed.
  remote_ = model::RemoteSnapshot{};
  remote_.ref = signin_ref_;
  remote_.token_source = signin_saved_to_.empty() ? model::TokenSource::SignedIn
                                                  : model::TokenSource::ConfigFile;
  remote_.token_origin = signin_saved_to_.empty() ? "this session" : config_path_;
  pipelines_ = model::PipelineSnapshot{};
  pulls_ = model::PullSnapshot{};
  jobs_ = model::JobList{};
  jobs_open_ = false;
  pull_details_open_ = false;
  pipeline_selected_ = 0;
  pull_selected_ = 0;
  last_pipeline_fetch_ = {};
}

void App::OpenSignInPage() {
  const std::string url = signin_stage_ == ui::SignInStage::Paste
                              ? remote::TokenPageUrl(signin_ref_, signin_origin_)
                              : (signin_code_.verification_uri_complete.empty()
                                     ? signin_code_.verification_uri
                                     : signin_code_.verification_uri_complete);
  signin_browser_failed_ = !remote::OpenInBrowser(url);
  if (signin_browser_failed_) {
    Note("no browser opener found on this machine", false);
  }
}

void App::CancelSignIn() {
  // Both are cancelled rather than only the one thought to be running: a device
  // request that lands after the overlay closes has nowhere to go, and the poll
  // chain has to be broken here or it re-arms itself forever.
  device_fetcher_.Cancel();
  poll_fetcher_.Cancel();

  const bool granted = signin_stage_ == ui::SignInStage::Granted;
  signin_code_ = remote::DeviceCode{};
  token_input_.clear();
  CloseOverlay();

  if (granted) {
    // Only now, so the fetch goes out authenticated and lands on a view the
    // user is actually looking at.
    switch (view_) {
      case ui::View::Remote:
        EnsureRemote();
        break;
      case ui::View::Pipelines:
        EnsurePipelines();
        break;
      case ui::View::Pulls:
        EnsurePulls();
        break;
      case ui::View::Status:
      case ui::View::History:
      case ui::View::Branches:
      case ui::View::Graph:
      case ui::View::Diff:
      case ui::View::Stashes:
      // Where the sign-in was very likely started from. It reads the new token
      // source out of App on the next frame and costs no request to do it.
      case ui::View::Settings:
        break;
    }
    Note("signed in to " + session_token_.host, false);
  }
}

ui::SignInView App::SignInViewState() const {
  ui::SignInView view;
  view.stage = signin_stage_;
  view.provider = signin_ref_.provider;
  view.host = signin_ref_.host;
  view.user_code = signin_code_.user_code;
  view.verification_uri = signin_code_.verification_uri;
  view.token_page_url = remote::TokenPageUrl(signin_ref_, signin_origin_);
  view.expires_at = signin_code_.expires_at;
  view.now = remote::NowSeconds();
  view.saved_to = signin_saved_to_;
  view.save_error = signin_save_error_;
  view.browser_failed = signin_browser_failed_;
  view.error = signin_error_;
  view.hint = signin_hint_;
  return view;
}

void App::SignOut() {
  DiscoverRemotes();
  const std::string host = remote_.ref.host;
  if (host.empty()) {
    Note("no remote to sign out of", false);
    return;
  }

  const bool had_session = session_token_.MatchesHost(host);
  if (had_session) {
    session_token_ = remote::SessionToken{};
  }

  // Removed rather than blanked: `token = ""` on disk is a line that reads like
  // a configured secret with the secret missing, and ResolveToken would have to
  // learn to tell those apart.
  const bool had_config = config_.Unset("hosts." + host + ".token");
  if (!had_session && !had_config) {
    Note("not signed in to " + host, false);
    return;
  }

  std::string error;
  if (had_config && !config_.Save(config_path_, &error)) {
    // The session copy is gone either way, so this run is anonymous from here.
    // What could not be done is the part on disk, and the next launch is when
    // that matters, so it is said now rather than discovered then.
    Note("signed out here, but the config file still has the token: " + error, true);
  } else {
    Note("signed out of " + host, false);
  }

  // Everything cached came back to a request that carried that token, so none
  // of it describes what an anonymous client can see.
  const model::RemoteRef ref = remote_.ref;
  remote_ = model::RemoteSnapshot{};
  remote_.ref = ref;
  const remote::Token token =
      remote::ResolveToken(remote_.ref, config_, config_path_, &session_token_);
  remote_.token_source = token.source;
  remote_.token_origin = token.origin;
  pipelines_ = model::PipelineSnapshot{};
  pulls_ = model::PullSnapshot{};
  jobs_ = model::JobList{};
  jobs_open_ = false;
  pull_details_open_ = false;
  pipeline_selected_ = 0;
  pull_selected_ = 0;
  last_pipeline_fetch_ = {};
}

ui::SettingsView App::SettingsViewState() const {
  ui::SettingsView view;
  view.selected = settings_selected_;

  view.provider = remote_.ref.provider;
  view.host = remote_.ref.host;
  view.token_source = remote_.token_source;
  view.token_origin = remote_.token_origin;

  view.repo_name = snapshot_.repo_name;
  // libgit2 hands back a workdir with a trailing slash, which makes the last
  // path segment empty — and PathText, which keeps the filename and gives up
  // the directory, then has nothing to keep and truncates the interesting end
  // away instead.
  view.repo_path = repo_.WorkdirPath();
  while (view.repo_path.size() > 1 && view.repo_path.back() == '/') {
    view.repo_path.pop_back();
  }
  view.branch = snapshot_.branch;
  view.upstream = snapshot_.has_upstream ? snapshot_.upstream : std::string{};
  view.head_detached = snapshot_.head_detached;

  view.remotes.reserve(remotes_.size());
  for (std::size_t i = 0; i < remotes_.size(); ++i) {
    // Through SafeUrl here rather than in the panel, because this is the last
    // point that has the raw one: a remote configured as https://user:token@…
    // must not reach anything under ui/ with the token still in it.
    view.remotes.push_back({remotes_[i].name, ui::SafeUrl(remotes_[i].url), i == remote_index_});
  }

  view.config_path = config_path_;
  view.config_exists = config_on_disk_;
  view.dirty = settings_dirty_;
  view.key_bindings = keys_.Count();
  view.key_overrides = keys_.UserCount();

  view.compact = compact_;
  view.splash = splash_;
  view.version = kVersion;
  return view;
}

void App::ActivateSetting() {
  // Resolved through the same builder the panel renders from, so this can never
  // act on a row other than the one under the cursor — the list changes shape
  // with the state it describes, and a second table of indices here would be
  // wrong the first time somebody signed in.
  const ui::SettingsRow row = ui::SettingsRowAt(SettingsViewState(), settings_selected_);

  switch (row.action) {
    case ui::SettingsAction::None:
      // A row that is inert for a reason answers with the reason. One that is
      // merely inert says nothing, because there is nothing to say.
      if (!row.blocked.empty()) {
        Note(row.blocked, false);
      }
      return;

    case ui::SettingsAction::SignIn:
      RequestSignIn();
      return;
    case ui::SettingsAction::SignOut:
      SignOut();
      return;
    case ui::SettingsAction::NextRemote:
      NextRemote();
      return;

    case ui::SettingsAction::NextTheme:
      // Through Perform so the `t` key and this row cannot drift apart — that
      // case is also where the depth caveat gets attached to the toast.
      Perform(ui::Action::Theme);
      return;

    case ui::SettingsAction::NextIcons: {
      // Cycling onto the nerd set is the opt-in the auto-detection is not
      // allowed to make on the user's behalf: a keypress is consent, and two
      // more presses undo it if the font turns out not to have the icons.
      const ui::GlyphMode order[] = {ui::GlyphMode::Ascii, ui::GlyphMode::Unicode,
                                     ui::GlyphMode::Nerd};
      int at = 0;
      for (int i = 0; i < 3; ++i) {
        if (order[i] == ui::GlyphModeNow()) {
          at = i;
        }
      }
      const ui::GlyphMode next = order[(at + 1) % 3];
      ui::SetGlyphMode(next);
      settings_dirty_ = true;
      Note("icons: " + ui::GlyphModeName(next), false);
      return;
    }

    case ui::SettingsAction::NextBorder: {
      const ui::PanelBorder order[] = {ui::PanelBorder::Rounded, ui::PanelBorder::Light,
                                       ui::PanelBorder::Heavy, ui::PanelBorder::Double};
      int at = 0;
      for (int i = 0; i < 4; ++i) {
        if (order[i] == ui::PanelBorderNow()) {
          at = i;
        }
      }
      const ui::PanelBorder next = order[(at + 1) % 4];
      ui::SetPanelBorder(next);
      settings_dirty_ = true;
      Note("border: " + ui::PanelBorderName(next), false);
      return;
    }

    case ui::SettingsAction::NextDepth: {
      const ui::ColorDepth order[] = {ui::ColorDepth::TrueColor, ui::ColorDepth::Ansi256,
                                      ui::ColorDepth::Ansi16, ui::ColorDepth::None};
      int at = 0;
      for (int i = 0; i < 4; ++i) {
        if (order[i] == ui::ColorDepthNow()) {
          at = i;
        }
      }
      const ui::ColorDepth next = order[(at + 1) % 4];
      // One call, not two: SetColorDepth is what pushes the decision into FTXUI
      // as well, and the two quantizers disagreeing is a bug already paid for.
      ui::SetColorDepth(next);
      settings_dirty_ = true;
      Note("colours: " + ui::ColorDepthName(next), false);
      return;
    }

    case ui::SettingsAction::ToggleAnimations: {
      const bool on = ui::ReducedMotion();  // currently reduced, so turning them on
      ui::SetReducedMotion(!on);
      settings_dirty_ = true;
      Note(std::string("animations: ") + (on ? "on" : "off"), false);
      return;
    }

    case ui::SettingsAction::ToggleCompact:
      compact_ = !compact_;
      settings_dirty_ = true;
      Note(std::string("compact layout: ") + (compact_ ? "on" : "off"), false);
      return;

    case ui::SettingsAction::ToggleSplash:
      splash_ = !splash_;
      settings_dirty_ = true;
      Note(std::string("startup card: ") + (splash_ ? "on" : "off") + "  (next launch)", false);
      return;

    case ui::SettingsAction::SaveConfig:
      SaveSettings();
      return;

    case ui::SettingsAction::OpenKeys:
      help_scroll_ = 0;
      OpenOverlay(kHelp);
      return;
  }
}

void App::SaveSettings() {
  config_.Set("theme.name", ui::ThemeName());
  config_.Set("theme.border", ui::PanelBorderName(ui::PanelBorderNow()));
  config_.Set("theme.animations", ui::ReducedMotion() ? "false" : "true");
  config_.Set("theme.splash", splash_ ? "true" : "false");
  config_.Set("layout.compact", compact_ ? "true" : "false");

  // "auto" whenever the resolved value is the one detection would have picked
  // anyway. Pinning it instead would freeze *this* terminal's answer into a file
  // that may well be read on another one — a dotfile carried to a 16-colour ssh
  // session would arrive there demanding truecolor, and to a plain xterm
  // demanding the nerd glyphs its font does not have.
  const bool depth_detected = ui::DetectColorDepth() == ui::ColorDepthNow();
  config_.Set("theme.depth",
              depth_detected ? "auto" : ui::ColorDepthKey(ui::ColorDepthNow()));
  const bool icons_detected = ui::DetectGlyphMode() == ui::GlyphModeNow();
  config_.Set("theme.icons",
              icons_detected ? "auto" : ui::GlyphModeName(ui::GlyphModeNow()));

  std::string error;
  if (!config_.Save(config_path_, &error)) {
    Note(error, true);
    return;
  }
  config_on_disk_ = true;
  settings_dirty_ = false;
  Note("saved to " + config_path_, false);
}

void App::CancelTransfer() {
  if (!transfer_fetcher_.Running()) {
    CloseOverlay();
    return;
  }
  // Returns immediately: the worker is inside a network call and the UI thread
  // cannot wait for it. The pane stays up, saying so, until the result lands.
  transfer_fetcher_.Cancel();
  transfer_cancelling_ = true;
}

void App::CollectTransfer() {
  git::TransferResult result;
  if (!transfer_fetcher_.Consume(&result)) {
    return;
  }

  const TransferKind kind = transfer_kind_;
  transfer_kind_ = TransferKind::None;
  transfer_cancelling_ = false;
  CloseOverlay();

  // Whether ssh actually came and collected the passphrase decides what a
  // failure means, so it has to be read before the server is torn down.
  const bool askpass_active = askpass_ != nullptr;
  const bool askpass_used = askpass_active && askpass_->served();
  askpass_.reset();
  git::ClearAskpassEnv();

  if (!result.ok) {
    if (result.summary == "cancelled") {
      Note("transfer cancelled", false);
      return;
    }
    // ssh asked for the passphrase, was given one, and still could not
    // authenticate. Dropping it is what makes the next attempt ask again rather
    // than fail the same way for the rest of the session.
    if (askpass_used) {
      ssh_passphrase_.clear();
      passphrase_rejected_ = true;
      Note("ssh could not use that passphrase", true);
      return;
    }
    Note(result.summary + (result.detail.empty() ? "" : " — " + result.detail), true);
    return;
  }

  // It worked and nothing ever asked for the passphrase, so it was not the
  // thing that authenticated. Keeping a secret that buys nothing is not free.
  if (askpass_active && !askpass_used) {
    ssh_passphrase_.clear();
  }

  std::string note = result.summary;
  if (!result.detail.empty()) {
    note += " · " + result.detail;
  }
  Note(note, false);

  // A pull can move HEAD and rewrite the working tree, and a push changes what
  // the branch view's ahead/behind counts mean. Both make every cached read of
  // the repository stale.
  Refresh();
  history_loaded_ = false;
  if (view_ != ui::View::Status) {
    EnsureHistory();
  }
  // A fetch only moves tracking refs, so ahead/behind changes but the remote's
  // own description does not: no reason to spend a request re-reading it.
  (void)kind;
}

ui::TransferView App::TransferViewState() const {
  const git::TransferProgress progress = transfer_sink_->Read();

  ui::TransferView view;
  switch (transfer_kind_) {
    case TransferKind::Fetch:
      view.title = "Fetching from " + (remotes_.empty() ? "" : remotes_[remote_index_].name);
      break;
    case TransferKind::Pull:
      view.title = "Pulling " + snapshot_.branch;
      break;
    case TransferKind::Push:
      view.title = "Pushing " + snapshot_.branch;
      break;
    case TransferKind::Prune:
      view.title = "Pruning " + (remotes_.empty() ? "" : remotes_[remote_index_].name);
      break;
    case TransferKind::None:
      view.title = "Working";
      break;
  }

  switch (progress.phase) {
    case git::TransferPhase::Connecting:
      view.phase = "connecting";
      break;
    case git::TransferPhase::Receiving:
      view.phase = "receiving objects";
      break;
    case git::TransferPhase::Resolving:
      view.phase = "resolving deltas";
      break;
    case git::TransferPhase::Sending:
      view.phase = "sending objects";
      break;
    case git::TransferPhase::Updating:
      view.phase = "updating the working tree";
      break;
    case git::TransferPhase::Done:
      view.phase = "done";
      break;
    case git::TransferPhase::Failed:
      view.phase = "failed";
      break;
    case git::TransferPhase::Idle:
      view.phase = "starting";
      break;
  }
  if (transfer_cancelling_) {
    view.phase = quit_after_transfer_ ? "cancelling, then quitting"
                                      : "cancelling — waiting for the request to give up";
  }

  view.detail = progress.remote_message;
  view.ratio = progress.ratio();
  view.bytes = progress.received_bytes;
  if (progress.phase == git::TransferPhase::Sending) {
    view.objects = progress.pushed_objects;
    view.total = progress.total_push_objects;
  } else {
    view.objects = progress.phase == git::TransferPhase::Resolving ? progress.indexed_objects
                                                                   : progress.received_objects;
    view.total = progress.total_objects;
  }
  view.cancellable = !transfer_cancelling_;
  return view;
}

void App::Move(int delta) {
  const int count = ActiveCount();
  if (count == 0) {
    return;
  }
  int& selection = ActiveSelection();
  const int before = selection;
  selection = std::clamp(selection + delta, 0, count - 1);

  // Moving off a run closes its jobs rather than fetching the next run's.
  // Following the cursor would put one request on every keystroke, which on an
  // anonymous GitHub budget is a dozen rows of scrolling and then nothing for
  // an hour. `enter` opens the new one, and that keypress is the consent.
  //
  // The pull request detail pane is deliberately not treated the same way: it
  // costs nothing to redraw for the row under the cursor.
  if (view_ == ui::View::Pipelines && jobs_open_ && selection != before) {
    jobs_open_ = false;
  }
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

  // The mirror is built from what was just read. `r` and the reload after a pull
  // both invalidate the history under an active filter, and a mirror left
  // describing the previous walk hands OpenSelectedDiff a commit id that may no
  // longer exist.
  RebuildFilter();
  commit_selected_ = std::clamp(commit_selected_, 0,
                                std::max(0, static_cast<int>(VisibleHistory().commits.size()) - 1));
  branch_selected_ = std::clamp(
      branch_selected_, 0, std::max(0, static_cast<int>(VisibleHistory().branches.size()) - 1));
}

void App::EnsureDiff() {
  if (diff_loaded_) {
    return;
  }
  diff_ = repo_.ReadDiff(diff_request_, kMaxDiffLines);
  diff_loaded_ = true;
  diff_line_ = std::clamp(diff_line_, 0, std::max(0, static_cast<int>(diff_.lines.size()) - 1));
  if (!diff_.error.empty()) {
    Note(diff_.error, true);
  }
}

void App::ShowDiff(const git::DiffRequest& request) {
  diff_request_ = request;
  diff_loaded_ = false;
  diff_line_ = 0;
  // A filter narrowing a list of files says nothing useful about a list of diff
  // lines, and carrying it across would silently hide most of the diff.
  filter_.clear();
  SetView(ui::View::Diff);
}

void App::OpenSelectedDiff() {
  if (view_ == ui::View::Status) {
    const model::StatusEntry* entry = Selected();
    if (entry == nullptr) {
      Note("nothing to diff", false);
      return;
    }
    git::DiffRequest request;
    // A staged row and an unstaged row for the same file are two different
    // changes, and showing the wrong one is worse than showing neither.
    request.source = entry->stage == model::Stage::Index ? model::DiffSource::Staged
                                                         : model::DiffSource::Worktree;
    request.path = entry->path;
    ShowDiff(request);
    return;
  }

  if (view_ == ui::View::History) {
    const model::HistorySnapshot& history = VisibleHistory();
    if (commit_selected_ < 0 || commit_selected_ >= static_cast<int>(history.commits.size())) {
      Note("nothing to diff", false);
      return;
    }
    git::DiffRequest request;
    request.source = model::DiffSource::Commit;
    request.commit = history.commits[static_cast<std::size_t>(commit_selected_)].id;
    ShowDiff(request);
    return;
  }

  Note("nothing to diff here", false);
}

void App::SwitchDiffSource() {
  if (diff_request_.source == model::DiffSource::Commit) {
    // There is no other side of a commit to show. Falling back to the working
    // tree would be a different diff entirely under the same keystroke.
    Note("this is a commit — there is no staged half of it", false);
    return;
  }
  git::DiffRequest request = diff_request_;
  request.source = request.source == model::DiffSource::Worktree ? model::DiffSource::Staged
                                                                 : model::DiffSource::Worktree;
  ShowDiff(request);
}

void App::JumpFile(int delta) {
  if (diff_.files.empty()) {
    return;
  }
  // The cursor is a line index and the file boundaries are line indices, so
  // the next file is the first boundary past where the cursor is standing.
  const auto here = static_cast<std::size_t>(std::max(0, diff_line_));
  if (delta > 0) {
    for (const model::DiffFile& file : diff_.files) {
      if (file.first_line > here) {
        diff_line_ = static_cast<int>(file.first_line);
        return;
      }
    }
    Note("last file", false);
    return;
  }
  for (auto it = diff_.files.rbegin(); it != diff_.files.rend(); ++it) {
    if (it->first_line < here) {
      diff_line_ = static_cast<int>(it->first_line);
      return;
    }
  }
  Note("first file", false);
}

void App::EnsureStashes() {
  if (stashes_loaded_) {
    return;
  }
  stashes_ = repo_.ReadStashes();
  stashes_loaded_ = true;
  RebuildFilter();
  stash_selected_ =
      std::clamp(stash_selected_, 0, std::max(0, static_cast<int>(stashes_.entries.size()) - 1));
}

const model::Stash* App::SelectedStash() const {
  const model::StashList& list = VisibleStashes();
  if (stash_selected_ < 0 || stash_selected_ >= static_cast<int>(list.entries.size())) {
    return nullptr;
  }
  return &list.entries[static_cast<std::size_t>(stash_selected_)];
}

void App::SaveStash() {
  if (snapshot_.clean()) {
    Note("nothing to stash", false);
    return;
  }
  // Untracked files go in too. Leaving them behind is git's default and it is
  // the one that surprises people: a "clean tree" with new files still in it is
  // not what anyone means by stashing their work.
  const git::OpResult result = repo_.StashSave(/*message=*/{}, /*include_untracked=*/true);
  stashes_loaded_ = false;
  if (result.ok) {
    EnsureStashes();
  }
  Apply(result);
}

void App::ApplyStash() {
  const model::Stash* stash = SelectedStash();
  if (stash == nullptr) {
    Note("no stash selected", false);
    return;
  }
  // No confirm: the entry survives an apply, so the worst case is a working
  // tree you can put back by hand. Pop and drop are the two that cannot.
  const git::OpResult result = repo_.StashApply(stash->index);
  stashes_loaded_ = false;
  EnsureStashes();
  Apply(result);
}

void App::RequestStash(ConfirmKind kind) {
  if (SelectedStash() == nullptr) {
    Note("no stash selected", false);
    return;
  }
  confirm_kind_ = kind;
  OpenOverlay(kConfirm);
}

void App::PerformStashPop() {
  CloseOverlay();
  const model::Stash* stash = SelectedStash();
  if (stash == nullptr) {
    return;
  }
  const git::OpResult result = repo_.StashPop(stash->index);
  stashes_loaded_ = false;
  EnsureStashes();
  Apply(result);
}

void App::PerformStashDrop() {
  CloseOverlay();
  const model::Stash* stash = SelectedStash();
  if (stash == nullptr) {
    return;
  }
  const git::OpResult result = repo_.StashDrop(stash->index);
  stashes_loaded_ = false;
  EnsureStashes();
  // Dropping the last entry leaves the cursor one past the end of the list.
  stash_selected_ =
      std::clamp(stash_selected_, 0, std::max(0, static_cast<int>(VisibleStashes().entries.size()) - 1));
  Apply(result);
}

void App::RequestRebase() {
  if (operation_.active()) {
    Note(std::string(model::OperationName(operation_.operation)) + " is already in progress",
         true);
    return;
  }
  confirm_kind_ = ConfirmKind::Rebase;
  OpenOverlay(kConfirm);
}

void App::PerformRebase() {
  CloseOverlay();
  const git::OpResult result = repo_.RebaseOntoUpstream();
  // A rebase rewrites the branch, so every cached read of it is wrong now —
  // including the diff, which Refresh drops on its own.
  history_loaded_ = false;
  Apply(result);
  if (result.ok && view_ != ui::View::Status) {
    EnsureHistory();
  }
}

void App::OpenOperation() {
  // Read again rather than trusting the cached one: this is the pane whose
  // whole job is to be right about what is happening on disk.
  operation_ = repo_.ReadOperation();
  if (!operation_.active()) {
    Note("nothing in progress", false);
    return;
  }
  OpenOverlay(kOperation);
}

void App::RequestOperation(ConfirmKind kind) {
  confirm_kind_ = kind;
  OpenOverlay(kConfirm);
}

void App::PerformOperationContinue() {
  CloseOverlay();
  const git::OpResult result = repo_.RebaseContinue();
  history_loaded_ = false;
  Apply(result);
  // Apply only refreshes on success, and a refusal here still leaves the
  // operation banner as the thing the user needs to see.
  operation_ = repo_.ReadOperation();
}

void App::PerformOperationAbort() {
  CloseOverlay();
  const git::OpResult result = repo_.RebaseAbort();
  history_loaded_ = false;
  Apply(result);
  operation_ = repo_.ReadOperation();
}

void App::SetView(ui::View view) {
  // A filter belongs to the list it was typed against. Carrying "readme" from
  // the file list onto the branch list hides nine branches for no reason the
  // user can see, since the box that explains it has already closed.
  if (view != view_) {
    filter_.clear();
  }
  view_ = view;

  // The ticker exists for the CI view's refresh interval and its countdown, so
  // it runs exactly while that view is on screen. Anywhere else it would be a
  // thread waking a terminal once a second to change nothing.
  if (view == ui::View::Pipelines) {
    ticker_.Start();
  } else {
    ticker_.Stop();
  }

  switch (view) {
    case ui::View::Remote:
      EnsureRemote();
      return;
    case ui::View::Pipelines:
      EnsurePipelines();
      return;
    case ui::View::Pulls:
      EnsurePulls();
      return;
    case ui::View::Diff:
      EnsureDiff();
      return;
    case ui::View::Stashes:
      EnsureStashes();
      return;
    // Nothing to fetch, but the account row is a fact about the remote and the
    // sidecar lists them, so the discovery that every network view pays for on
    // its way in has to happen here too.
    case ui::View::Settings:
      DiscoverRemotes();
      return;
    case ui::View::Status:
      return;
    case ui::View::History:
    case ui::View::Branches:
    case ui::View::Graph:
      break;
  }
  EnsureHistory();
}

void App::Reload() {
  // On the network views `r` means the network, everywhere else it means the
  // repository. Refreshing whichever one is not on screen is a surprise.
  if (view_ == ui::View::Pipelines) {
    if (pipeline_fetcher_.Running()) {
      Note("already fetching", false);
      return;
    }
    if (!remote_.ref.valid()) {
      Note("no remote to fetch", true);
      return;
    }
    StartPipelineFetch();
    Note("refreshing CI", false);
    return;
  }

  if (view_ == ui::View::Pulls) {
    if (pull_fetcher_.Running()) {
      Note("already fetching", false);
      return;
    }
    if (!remote_.ref.valid()) {
      Note("no remote to fetch", true);
      return;
    }
    StartPullFetch();
    Note("refreshing pull requests", false);
    return;
  }

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
  diff_loaded_ = false;
  stashes_loaded_ = false;
  if (view_ == ui::View::Diff) {
    EnsureDiff();
  } else if (view_ == ui::View::Stashes) {
    EnsureStashes();
  } else if (view_ != ui::View::Status) {
    EnsureHistory();
  }
  Note("re-read the repository", false);
}

const model::StatusEntry* App::Selected() const {
  // Through the filtered view, because the cursor counts rows on screen. Every
  // caller acts on the row the user is looking at.
  const model::StatusSnapshot& status = VisibleStatus();
  if (selected_ < 0 || selected_ >= static_cast<int>(status.entries.size())) {
    return nullptr;
  }
  return &status.entries[static_cast<std::size_t>(selected_)];
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
  confirm_kind_ = ConfirmKind::Discard;
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

ui::Scope App::CurrentScope() const {
  switch (view_) {
    case ui::View::Status:
      return ui::Scope::Status;
    case ui::View::Graph:
      return ui::Scope::Graph;
    case ui::View::Diff:
      return ui::Scope::Diff;
    case ui::View::Stashes:
      return ui::Scope::Stash;
    case ui::View::Branches:
      return ui::Scope::Branches;
    case ui::View::History:
    case ui::View::Remote:
    case ui::View::Pipelines:
    case ui::View::Pulls:
    // Claims no keys of its own. Everything it does is on `enter`, which is
    // already the global "act on this row", so a scope here would only be a
    // second place for a binding to hide.
    case ui::View::Settings:
      break;
  }
  return ui::Scope::Global;
}

bool App::Perform(ui::Action action) {
  const std::vector<ui::View>& views = ui::AllViews();
  const auto index_of = [&views](ui::View view) {
    for (std::size_t i = 0; i < views.size(); ++i) {
      if (views[i] == view) {
        return i;
      }
    }
    return std::size_t{0};
  };

  switch (action) {
    case ui::Action::None:
      return false;

    case ui::Action::Quit:
      return false;  // handled by the caller, which owns the screen

    case ui::Action::Help:
      help_scroll_ = 0;
      OpenOverlay(kHelp);
      return true;

    case ui::Action::Reload:
      Reload();
      return true;

    case ui::Action::Theme: {
      const std::string label = ui::NextTheme();
      // The settings page's save row reports this, and `t` is the same change
      // whether it was pressed there or anywhere else.
      settings_dirty_ = true;
      // Below truecolor two palettes can quantize onto the same indices, and a
      // theme switch that changes nothing on screen reads as a broken key
      // rather than as a terminal limit. Naming the depth is the difference
      // between "this is broken" and "this terminal cannot show it".
      const ui::ColorDepth depth = ui::ColorDepthNow();
      const std::string note = depth == ui::ColorDepth::TrueColor
                                   ? "theme: " + label
                                   : "theme: " + label + "  (" + ui::ColorDepthName(depth) + ")";
      Note(note, false);
      return true;
    }

    case ui::Action::NextView:
      SetView(views[(index_of(view_) + 1) % views.size()]);
      return true;

    case ui::Action::PrevView:
      SetView(views[(index_of(view_) + views.size() - 1) % views.size()]);
      return true;

    // Positional, so `[layout] views` reorders what the digits reach and the
    // number printed on a tab is always the key that gets to it. A slot past
    // the end of a shortened list is a key with nothing behind it.
    case ui::Action::View1:
    case ui::Action::View2:
    case ui::Action::View3:
    case ui::Action::View4:
    case ui::Action::View5:
    case ui::Action::View6:
    case ui::Action::View7:
    case ui::Action::View8:
    case ui::Action::View9:
    case ui::Action::View10: {
      const auto slot = static_cast<std::size_t>(action) -
                        static_cast<std::size_t>(ui::Action::View1);
      if (slot < views.size()) {
        SetView(views[slot]);
      }
      return true;
    }

    case ui::Action::Down:
      Move(1);
      return true;
    case ui::Action::Up:
      Move(-1);
      return true;
    case ui::Action::PageDown:
      Move(10);
      return true;
    case ui::Action::PageUp:
      Move(-10);
      return true;
    case ui::Action::First:
      SelectFirst();
      return true;
    case ui::Action::Last:
      SelectLast();
      return true;

    case ui::Action::Open:
      if (view_ == ui::View::Pipelines) {
        ToggleJobs();
        return true;
      }
      if (view_ == ui::View::Pulls) {
        ToggleDetails();
        return true;
      }
      if (view_ == ui::View::Settings) {
        ActivateSetting();
        return true;
      }
      // Everywhere else, opening a row means looking at what changed in it.
      OpenSelectedDiff();
      return true;

    case ui::Action::Filter:
      OpenFilter();
      return true;

    case ui::Action::NextRemote:
      NextRemote();
      return true;
    case ui::Action::SignIn:
      RequestSignIn();
      return true;
    case ui::Action::Prune:
      RequestPrune();
      return true;
    case ui::Action::Fetch:
      StartTransfer(TransferKind::Fetch);
      return true;
    case ui::Action::Pull:
      StartTransfer(TransferKind::Pull);
      return true;
    case ui::Action::Push:
      RequestPush();
      return true;

    case ui::Action::ToggleStage:
      ToggleStage();
      return true;
    case ui::Action::Stage:
      StageSelected();
      return true;
    case ui::Action::Unstage:
      UnstageSelected();
      return true;
    case ui::Action::StageAll:
      StageEverything();
      return true;
    case ui::Action::Discard:
      RequestDiscard();
      return true;
    case ui::Action::Commit:
      OpenCommit();
      return true;

    case ui::Action::DiffSwitch:
      SwitchDiffSource();
      return true;
    case ui::Action::DiffNextFile:
      JumpFile(1);
      return true;
    case ui::Action::DiffPrevFile:
      JumpFile(-1);
      return true;

    case ui::Action::StashSave:
      SaveStash();
      return true;
    case ui::Action::StashApply:
      ApplyStash();
      return true;
    case ui::Action::StashPop:
      RequestStash(ConfirmKind::StashPop);
      return true;
    case ui::Action::StashDrop:
      RequestStash(ConfirmKind::StashDrop);
      return true;

    case ui::Action::Rebase:
      RequestRebase();
      return true;
    case ui::Action::Operation:
      OpenOperation();
      return true;

    case ui::Action::PanLeft:
      PanGraph(std::max(1, ui::GraphWindow(graph_.bucket) / 8));
      return true;
    case ui::Action::PanRight:
      PanGraph(-std::max(1, ui::GraphWindow(graph_.bucket) / 8));
      return true;
    case ui::Action::BucketDay:
      SetBucket(ui::Bucket::Day);
      return true;
    case ui::Action::BucketWeek:
      SetBucket(ui::Bucket::Week);
      return true;
    case ui::Action::BucketMonth:
      SetBucket(ui::Bucket::Month);
      return true;
    case ui::Action::GraphOldest:
      graph_.offset = MaxGraphOffset();
      return true;
    case ui::Action::GraphNewest:
      graph_.offset = 0;
      return true;
  }
  return false;
}

std::vector<Box>* App::ActiveRowBoxes() {
  switch (view_) {
    case ui::View::Status:
    case ui::View::History:
    case ui::View::Branches:
    case ui::View::Pipelines:
    case ui::View::Pulls:
    case ui::View::Stashes:
    case ui::View::Settings:
      return &row_boxes_;
    case ui::View::Graph:
    case ui::View::Remote:
    // The diff scrolls but has no rows worth clicking: a line of context is not
    // a thing you select, and reflecting twenty thousand boxes to find that out
    // would cost more than the whole panel.
    case ui::View::Diff:
      break;
  }
  return nullptr;  // nothing selectable to click on
}

bool App::HandleMouse(const Mouse& mouse) {
  // The boxes were filled by the previous render. FTXUI runs a render before it
  // reads input, so on the first event they are already current.
  if (mouse.button == Mouse::WheelUp) {
    if (view_ == ui::View::Graph) {
      PanGraph(std::max(1, ui::GraphWindow(graph_.bucket) / 8));
    } else {
      Move(-kWheelRows);
    }
    return true;
  }
  if (mouse.button == Mouse::WheelDown) {
    if (view_ == ui::View::Graph) {
      PanGraph(-std::max(1, ui::GraphWindow(graph_.bucket) / 8));
    } else {
      Move(kWheelRows);
    }
    return true;
  }

  if (mouse.button != Mouse::Left || mouse.motion != Mouse::Pressed) {
    return false;
  }

  for (std::size_t i = 0; i < tab_boxes_.size(); ++i) {
    if (tab_boxes_[i].Contain(mouse.x, mouse.y)) {
      SetView(ui::AllViews()[i]);
      return true;
    }
  }

  std::vector<Box>* rows = ActiveRowBoxes();
  if (rows == nullptr) {
    return false;
  }
  // A row scrolled out of its frame still gets a box, and that box can land on
  // a coordinate inside some other panel. Requiring the hit to be inside the
  // body as well is what keeps a click on the footer from selecting row 300.
  if (!body_box_.Contain(mouse.x, mouse.y)) {
    return false;
  }
  for (std::size_t i = 0; i < rows->size(); ++i) {
    if (!(*rows)[i].Contain(mouse.x, mouse.y)) {
      continue;
    }
    const int index = static_cast<int>(i);
    int& selection = ActiveSelection();
    // A second click on the row already under the cursor opens it, which is the
    // closest thing a terminal has to a double click without guessing at
    // timings the terminal never reports.
    if (selection == index) {
      Perform(ui::Action::Open);
    } else {
      Move(index - selection);
    }
    return true;
  }
  return false;
}

// Posted from the workers to wake the event loop. Named special events rather
// than keystrokes, so nothing in the key routing can collide with them, and one
// per source so a wake-up says which result to go and look for.
const Event kRemoteReady = Event::Special("gittop:remote-ready");
const Event kPipelinesReady = Event::Special("gittop:pipelines-ready");
const Event kJobsReady = Event::Special("gittop:jobs-ready");
const Event kPullsReady = Event::Special("gittop:pulls-ready");
const Event kTransferDone = Event::Special("gittop:transfer-done");
const Event kDeviceCodeReady = Event::Special("gittop:device-code-ready");
const Event kPollReady = Event::Special("gittop:poll-ready");
const Event kTick = Event::Special("gittop:tick");

int App::Run() {
  Refresh();

  // Skipped when stdout is not a terminal. A splash is a thing to look at, and
  // there is nobody looking at a redirected stream — it would only be a second
  // and a half of escape codes in whatever is reading the output.
  if (splash_ && isatty(STDOUT_FILENO) == 1) {
    splash_until_ = std::chrono::steady_clock::now() +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<float>(kSplashSeconds));
  }

  auto screen = ScreenInteractive::Fullscreen();

  // Every one of these captures the screen by reference, which is why all of
  // them are shut down before it goes out of scope at the end of this function.
  fetcher_.SetNotifier([&screen] { screen.PostEvent(kRemoteReady); });
  pipeline_fetcher_.SetNotifier([&screen] { screen.PostEvent(kPipelinesReady); });
  job_fetcher_.SetNotifier([&screen] { screen.PostEvent(kJobsReady); });
  pull_fetcher_.SetNotifier([&screen] { screen.PostEvent(kPullsReady); });
  transfer_fetcher_.SetNotifier([&screen] { screen.PostEvent(kTransferDone); });
  device_fetcher_.SetNotifier([&screen] { screen.PostEvent(kDeviceCodeReady); });
  poll_fetcher_.SetNotifier([&screen] { screen.PostEvent(kPollReady); });
  ticker_.SetNotifier([&screen] { screen.PostEvent(kTick); });

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
                   text(std::string("  ") + ui::glyphs().prompt + " ") |
                       color(ui::theme().staged),
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
    switch (confirm_kind_) {
      case ConfirmKind::Push: {
        const std::string remote_name =
            remotes_.empty() ? "the remote" : remotes_[remote_index_].name;
        // Through SafeUrl, not raw: a remote configured with a token in its URL
        // must not print it here any more than it does on the remote panel.
        return ui::ConfirmPane("Push " + snapshot_.branch + " to " + remote_name + "?",
                               remotes_.empty() ? "" : ui::SafeUrl(remotes_[remote_index_].url),
                               /*warning=*/"", "push");
      }

      case ConfirmKind::StashPop: {
        const model::Stash* stash = SelectedStash();
        return ui::ConfirmPane("Pop this stash?",
                               stash == nullptr ? "" : stash->summary,
                               "The entry goes away once it applies cleanly.", "pop");
      }

      case ConfirmKind::StashDrop: {
        const model::Stash* stash = SelectedStash();
        return ui::ConfirmPane("Drop this stash?", stash == nullptr ? "" : stash->summary,
                               "This cannot be undone.", "drop");
      }

      case ConfirmKind::Rebase:
        // Named rather than counted: gittop does not know how many commits are
        // about to be replayed without a walk it has not done, and a confirm
        // that guesses is worse than one that is precise about what it knows.
        return ui::ConfirmPane("Rebase " + snapshot_.branch + " onto its upstream?",
                               snapshot_.branch,
                               "This rewrites commits that are already on this branch.",
                               "rebase");

      case ConfirmKind::OperationContinue:
        return ui::ConfirmPane(
            std::string("Continue the ") + model::OperationName(operation_.operation) + "?",
            operation_.detail, /*warning=*/"", "continue");

      case ConfirmKind::OperationAbort:
        return ui::ConfirmPane(
            std::string("Abort the ") + model::OperationName(operation_.operation) + "?",
            operation_.detail,
            "Everything the operation has done so far is discarded.", "abort");

      case ConfirmKind::Prune: {
        const std::string remote_name =
            remotes_.empty() ? "the remote" : remotes_[remote_index_].name;
        return ui::ConfirmPane(
            "Prune remote-tracking refs for " + remote_name + "?",
            remote_name + "/* refs whose branch is gone on the server",
            "Your local branches are not touched.", "prune");
      }

      case ConfirmKind::Discard:
        break;
    }
    return ui::ConfirmPane(discard_target_.change == model::Change::Untracked
                               ? "Delete this file?"
                               : "Discard these changes?",
                           discard_target_.path, "This cannot be undone.", "discard");
  });

  // ------------------------------------------------------------ help overlay
  auto help_pane = Renderer([this, &screen] {
    return ui::HelpPane(keys_, screen.dimx(), screen.dimy(), help_scroll_);
  });

  // -------------------------------------------------------- transfer overlay
  auto transfer_pane = Renderer([this] { return ui::TransferPane(TransferViewState(), spinner_); });

  // ------------------------------------------------------- passphrase overlay
  InputOption passphrase_option;
  passphrase_option.multiline = false;
  // The reason this overlay can exist at all without breaking the rule that
  // nothing under ui/ handles a secret: FTXUI renders asterisks, so the element
  // handed to PassphrasePane carries no passphrase in it.
  passphrase_option.password = true;
  passphrase_option.on_enter = [this] { SubmitPassphrase(); };
  auto passphrase_input =
      Input(&passphrase_input_, "passphrase", passphrase_option);

  auto passphrase_pane = Renderer(passphrase_input, [this, passphrase_input] {
    return ui::PassphrasePane(passphrase_input->Render(), passphrase_rejected_);
  });

  // ----------------------------------------------------------- filter overlay
  InputOption filter_option;
  filter_option.multiline = false;
  // Live rather than on submit: the count in the pane's corner is the reason
  // this is a box you type into instead of a prompt you answer, and it only
  // means anything if the list behind it is already narrowing.
  filter_option.on_change = [this] { RebuildFilter(); };
  filter_option.on_enter = [this] { CloseFilter(/*keep=*/true); };
  auto filter_input = Input(&filter_, "type to narrow the list", filter_option);

  auto filter_pane = Renderer(filter_input, [this, filter_input] {
    return ui::FilterPane(filter_input->Render(), ui::ViewName(view_), FilterMatches(),
                          FilterTotal());
  });

  // -------------------------------------------------------- operation overlay
  auto operation_pane = Renderer([this] { return ui::OperationPane(operation_); });

  // ---------------------------------------------------------- sign-in overlay
  InputOption token_option;
  token_option.multiline = false;
  // A pasted token is a secret in exactly the way a passphrase is, so it is
  // collected the same way: FTXUI renders asterisks, and SignInPane is handed
  // the rendered element rather than the string behind it.
  token_option.password = true;
  token_option.on_enter = [this] { SubmitPastedToken(); };
  auto token_input = Input(&token_input_, "paste the token", token_option);

  auto signin_pane = Renderer(token_input, [this, token_input] {
    return ui::SignInPane(SignInViewState(), token_input->Render(), spinner_);
  });

  auto overlay = Container::Tab({commit_pane, confirm_pane, help_pane, transfer_pane,
                                 passphrase_pane, filter_pane, operation_pane, signin_pane},
                                &overlay_index_);

  // --------------------------------------------------------------- main view
  auto main_view = Renderer([this, &screen] {
    ThrottleFrame();
    Tick();
    if (Animating()) {
      animation_pending_ = true;
      animation::RequestAnimationFrame();
    }

    // Reading the live terminal size here is what makes the layout responsive:
    // the dom has no way to ask, but the screen does.
    const int width = screen.dimx();
    const int height = screen.dimy();

    if (const float reveal = SplashReveal(); reveal > 0.0F) {
      return ui::Splash(snapshot_.repo_name, kVersion, reveal, width, height);
    }

    Elements body{ui::Header(snapshot_), ui::TabBar(view_, width, &tab_boxes_)};

    // Only the view on screen fills these, so a stale box from another view can
    // never be hit-tested against.
    row_boxes_.clear();

    Element panel;
    switch (view_) {
      case ui::View::Status:
        // Above the summary rather than below it: what the repository is in the
        // middle of outranks how many files are staged, and a banner under the
        // cards is a banner nobody reads before acting.
        body.push_back(ui::OperationBanner(operation_, keys_));
        body.push_back(ui::SummaryRow(VisibleStatus(), bars_,
                                      width < 84 || compact_));
        {
          // Below this the change list would be narrower than the paths it
          // holds, and a truncated path is worth less than the sidecar it
          // bought. The threshold is the sidecar's width plus the width a file
          // row needs before it starts eliding, not a round number.
          const int sidebar = ui::StatusSidebarWidth();
          const bool wide = width >= sidebar + 62 && !compact_;
          const int list_width = wide ? width - sidebar : width;
          Element list = ui::FileList(VisibleStatus(), selected_, list_width, &row_boxes_);
          if (wide) {
            panel = hbox({
                        std::move(list) | flex,
                        ui::StatusSidebar(snapshot_),
                    }) |
                    flex;
          } else {
            panel = std::move(list) | flex;
          }
        }
        break;

      case ui::View::History: {
        // The heatmap costs ten rows. On a short terminal the log is worth
        // more than the graph, so it goes first and the heatmap steps aside.
        if (height >= 30 && !compact_) {
          body.push_back(ui::Panel("ACTIVITY", ui::ActivityPanel(history_),
                                   {.focused = false}));
        }
        panel = ui::Panel("COMMITS",
                          ui::CommitList(VisibleHistory(), commit_selected_, width,
                                         &row_boxes_)) |
                flex;
        break;
      }

      case ui::View::Branches:
        panel = ui::Panel("BRANCHES",
                          ui::BranchList(VisibleHistory(), branch_selected_, width,
                                         &row_boxes_)) |
                flex;
        break;

      case ui::View::Diff: {
        ui::DiffView diff_view;
        diff_view.selected = diff_line_;
        diff_view.switchable = diff_request_.source != model::DiffSource::Commit;
        // Chrome is header 1 + tabs 1 + footer 2 + the panel's own frame and
        // title rows; what is left is how many diff lines actually fit.
        panel = ui::DiffPanel(diff_, diff_view, width, std::max(4, height - 8));
        break;
      }

      case ui::View::Stashes:
        panel = ui::StashList(VisibleStashes(), stash_selected_, &row_boxes_);
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
        panel = ui::RemotePanel(remote_, width, height, spinner_, git::TransportSummary());
        break;

      case ui::View::Pipelines:
        panel = ui::PipelinePanel(VisiblePipelines(), jobs_, remote_.ref, PipelineViewState(),
                                  width, height, spinner_, &row_boxes_);
        break;

      case ui::View::Pulls: {
        ui::PullView pull_view;
        pull_view.selected = pull_selected_;
        pull_view.details_open = pull_details_open_;
        panel = ui::PullPanel(VisiblePulls(), remote_.ref, pull_view, width, height, spinner_,
                              &row_boxes_);
        break;
      }

      case ui::View::Settings:
        panel = ui::SettingsPanel(SettingsViewState(), width, &row_boxes_);
        break;
    }

    if (panel) {
      // The panel's own box, used to reject a click that landed on a row whose
      // reflected geometry is outside the frame it was clipped to.
      body.push_back(std::move(panel) | reflect(body_box_));
    }

    body.push_back(ui::Footer(message_, message_is_error_, ToastFade(), view_, keys_, filter_));

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
    // The worker threads post these after a task finishes. They carry no data
    // themselves — they only wake the loop so the result can be picked up here,
    // on the UI thread, where every other piece of state is touched.
    if (event == kRemoteReady) {
      CollectFetch();
      return true;
    }
    if (event == kPipelinesReady) {
      CollectPipelines();
      return true;
    }
    if (event == kJobsReady) {
      CollectJobs();
      return true;
    }
    if (event == kPullsReady) {
      CollectPulls();
      return true;
    }
    if (event == kTransferDone) {
      CollectTransfer();
      // The worker has reported and been joined by now, so this is the first
      // moment it is safe to tear the screen down.
      if (quit_after_transfer_) {
        screen.Exit();
      }
      return true;
    }
    if (event == kDeviceCodeReady) {
      CollectDeviceCode();
      return true;
    }
    if (event == kPollReady) {
      CollectPoll();
      return true;
    }
    // One a second while the CI view is open. It repaints the countdown and
    // asks whether the interval is up; the answer is usually no.
    if (event == kTick) {
      MaybeAutoRefresh();
      return true;
    }

    // Before every other key, and it swallows the one that closed it. A splash
    // that hands `q` through to the dashboard is a splash that quits the program
    // when somebody taps a key to get rid of it.
    if ((event.is_character() || event.is_mouse() || event == Event::Escape ||
         event == Event::Return) &&
        DismissSplash()) {
      return true;
    }

    // Overlay routing lives here rather than on the panes themselves.
    // Container::Tab drops events unless it is focused, and the confirm, help
    // and transfer panes are plain Renderers with nothing focusable inside
    // them, so handlers attached to those panes never ran at all.
    if (overlay_open_) {
      switch (overlay_index_) {
        case kHelp: {
          // The movement keys scroll rather than close, because on a terminal
          // too narrow for two columns the bottom half of the help is only
          // reachable this way. Everything else still closes on one keystroke.
          const ui::Action moved = keys_.Lookup(ui::Scope::Global, event);
          if (moved == ui::Action::Down || moved == ui::Action::PageDown) {
            help_scroll_ += moved == ui::Action::Down ? 1 : 10;
            return true;
          }
          if (moved == ui::Action::Up || moved == ui::Action::PageUp) {
            help_scroll_ = std::max(0, help_scroll_ - (moved == ui::Action::Up ? 1 : 10));
            return true;
          }
          if (event.is_character() || event == Event::Escape || event == Event::Return) {
            CloseOverlay();
            return true;
          }
          return false;
        }

        case kTransfer:
          // Escape before Quit, because esc is one of quit's default keys and
          // the two mean different things here: esc stops the transfer and
          // stays, q stops it and leaves.
          if (event == Event::Escape) {
            CancelTransfer();
            return true;
          }
          if (keys_.Lookup(ui::Scope::Global, event) == ui::Action::Quit) {
            CancelTransfer();
            quit_after_transfer_ = true;
            return true;
          }
          // Nothing else can be answered here, and a stray key must not fall
          // through to the dashboard underneath while a push is in flight.
          return !event.is_mouse();

        case kFilter:
          // esc clears rather than merely closing. A filter you cannot see the
          // box for is a list quietly hiding rows, and the footer chip is a
          // reminder rather than a way out.
          if (event == Event::Escape) {
            CloseFilter(/*keep=*/false);
            return true;
          }
          return false;  // the Input takes the rest

        case kOperation:
          if (event == Event::Escape) {
            CloseOverlay();
            return true;
          }
          // Both answers go on to the confirm pane rather than acting here.
          // Neither is a keystroke that should be one keystroke.
          //
          // `c` only exists for a rebase. A merge or a cherry-pick is finished
          // by committing, and the pane says so rather than offering a key that
          // would come back with "no rebase in progress".
          if ((event == Event::Character('c') || event == Event::Character('C')) &&
              operation_.operation == model::Operation::Rebase) {
            RequestOperation(ConfirmKind::OperationContinue);
            return true;
          }
          if (event == Event::Character('a') || event == Event::Character('A')) {
            RequestOperation(ConfirmKind::OperationAbort);
            return true;
          }
          return event.is_character();

        case kConfirm:
          if (event == Event::Character('y') || event == Event::Character('Y')) {
            switch (confirm_kind_) {
              case ConfirmKind::Push:
                PerformPush();
                break;
              case ConfirmKind::StashPop:
                PerformStashPop();
                break;
              case ConfirmKind::StashDrop:
                PerformStashDrop();
                break;
              case ConfirmKind::Rebase:
                PerformRebase();
                break;
              case ConfirmKind::OperationContinue:
                PerformOperationContinue();
                break;
              case ConfirmKind::OperationAbort:
                PerformOperationAbort();
                break;
              case ConfirmKind::Prune:
                PerformPrune();
                break;
              case ConfirmKind::Discard:
                PerformDiscard();
                break;
            }
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

        case kPassphrase:
          // esc here abandons the transfer that the prompt is standing in front
          // of, not just the prompt: there is nothing to go back to.
          if (event == Event::Escape) {
            CancelPassphrase();
            return true;
          }
          return false;  // the Input takes the rest

        case kSignIn:
          if (event == Event::Escape) {
            CancelSignIn();
            return true;
          }
          // enter closes the pane once the token has landed, which is the only
          // thing left to do with it.
          if (event == Event::Return && signin_stage_ == ui::SignInStage::Granted) {
            CancelSignIn();
            return true;
          }
          if ((event == Event::Character('r') || event == Event::Character('R')) &&
              signin_stage_ == ui::SignInStage::Failed) {
            RequestSignIn();
            return true;
          }
          // Two keys for one command, because the Paste stage has a focused
          // Input and a pane that steals letters out of it is a pane that
          // silently corrupts a pasted token. `o` is safe while there is
          // nothing to type into; ctrl-o is safe in both, and is the one the
          // Paste stage advertises.
          if (event == Event::CtrlO ||
              ((event == Event::Character('o') || event == Event::Character('O')) &&
               signin_stage_ == ui::SignInStage::Waiting)) {
            OpenSignInPage();
            return true;
          }
          // Only the Paste stage has anything to type into. Everywhere else a
          // stray keystroke is swallowed rather than falling through to the
          // dashboard behind the overlay.
          return signin_stage_ != ui::SignInStage::Paste && event.is_character();

        case kCommit:
        default:
          if (event == Event::Escape) {
            CloseOverlay();
            return true;
          }
          return false;  // the Input takes the rest
      }
    }

    if (event.is_mouse()) {
      // FTXUI's mouse() accessor is non-const, and the handler is handed a
      // const Event. A copy is a few bytes and beats casting the constness off.
      Event copy = event;
      return HandleMouse(copy.mouse());
    }

    // One table lookup rather than thirty comparisons, which is what lets the
    // config move a key: the scope decides whether the graph's `d` or the
    // status view's is the one this keypress means.
    const ui::Action action = keys_.Lookup(CurrentScope(), event);
    if (action == ui::Action::Quit) {
      screen.Exit();
      return true;
    }
    return Perform(action);
  });

  screen.Loop(root);

  // Before `screen` goes out of scope, because every notifier captured it by
  // reference. A worker still in a ten-second timeout would otherwise post an
  // event into a destroyed screen on its way out.
  ticker_.Stop();
  fetcher_.Shutdown();
  pipeline_fetcher_.Shutdown();
  job_fetcher_.Shutdown();
  pull_fetcher_.Shutdown();
  transfer_fetcher_.Shutdown();
  device_fetcher_.Shutdown();
  poll_fetcher_.Shutdown();
  return 0;
}

}  // namespace gittop
