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

#include "remote/client.hpp"
#include "remote/http.hpp"
#include "remote/pipelines.hpp"
#include "remote/provider.hpp"
#include "remote/pulls.hpp"
#include "remote/token.hpp"
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
    case ui::View::Pipelines:
      return static_cast<int>(pipelines_.runs.size());
    case ui::View::Pulls:
      return static_cast<int>(pulls_.pulls.size());
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
  const remote::Token token = remote::ResolveToken(remote_.ref, config_, config_path_);
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

  const remote::Token token = remote::ResolveToken(remote_.ref, config_, config_path_);
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
    case ui::View::Status:
    case ui::View::History:
    case ui::View::Branches:
    case ui::View::Graph:
      break;
  }
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

  const remote::Token token = remote::ResolveToken(remote_.ref, config_, config_path_);
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
  pipeline_selected_ = std::clamp(pipeline_selected_, 0,
                                  std::max(0, static_cast<int>(pipelines_.runs.size()) - 1));

  if (pipelines_.state == model::FetchState::Failed) {
    Note(pipelines_.error, true);
    return;
  }

  // A refresh that arrives under an open drill-down re-reads the jobs, but only
  // while the run is still going. A finished run's jobs will not change again,
  // and re-asking every twenty seconds would spend the budget the poll interval
  // was chosen to protect.
  if (jobs_open_ && pipeline_selected_ < static_cast<int>(pipelines_.runs.size())) {
    const model::Pipeline& run = pipelines_.runs[static_cast<std::size_t>(pipeline_selected_)];
    if (jobs_.pipeline_id != run.id || !model::RunFinished(run.status)) {
      StartJobFetch();
    }
  }
}

void App::StartJobFetch() {
  if (!remote_.ref.valid() || job_fetcher_.Running()) {
    return;
  }
  if (pipeline_selected_ < 0 || pipeline_selected_ >= static_cast<int>(pipelines_.runs.size())) {
    return;
  }
  const model::Pipeline& run = pipelines_.runs[static_cast<std::size_t>(pipeline_selected_)];
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

  const remote::Token token = remote::ResolveToken(remote_.ref, config_, config_path_);
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
  if (pipelines_.runs.empty()) {
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

  const remote::Token token = remote::ResolveToken(remote_.ref, config_, config_path_);
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
  pull_selected_ =
      std::clamp(pull_selected_, 0, std::max(0, static_cast<int>(pulls_.pulls.size()) - 1));
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

  // The token is resolved here and lives only inside the task, exactly as the
  // HTTP fetches do. It is never stored on App and never reaches ui/.
  const remote::Token token = remote::ResolveToken(remote_.ref, config_, config_path_);
  git::Credentials credentials;
  if (token.present()) {
    credentials.username = UsernameFor(remote_.ref.provider);
    credentials.password = token.value;
  }

  const std::string path = repo_.WorkdirPath();
  std::shared_ptr<git::ProgressSink> sink = transfer_sink_;
  sink->Publish(git::TransferProgress{});

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

  if (!result.ok) {
    if (result.summary == "cancelled") {
      Note("transfer cancelled", false);
      return;
    }
    Note(result.summary + (result.detail.empty() ? "" : " — " + result.detail), true);
    return;
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

  commit_selected_ =
      std::clamp(commit_selected_, 0, std::max(0, static_cast<int>(history_.commits.size()) - 1));
  branch_selected_ =
      std::clamp(branch_selected_, 0, std::max(0, static_cast<int>(history_.branches.size()) - 1));
}

void App::SetView(ui::View view) {
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
    case ui::View::History:
    case ui::View::Branches:
    case ui::View::Remote:
    case ui::View::Pipelines:
    case ui::View::Pulls:
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
      OpenOverlay(kHelp);
      return true;

    case ui::Action::Reload:
      Reload();
      return true;

    case ui::Action::Theme: {
      const std::string label = ui::NextTheme();
      Note("theme: " + label, false);
      return true;
    }

    case ui::Action::NextView:
      SetView(views[(index_of(view_) + 1) % views.size()]);
      return true;

    case ui::Action::PrevView:
      SetView(views[(index_of(view_) + views.size() - 1) % views.size()]);
      return true;

    case ui::Action::ViewStatus:
      SetView(ui::View::Status);
      return true;
    case ui::Action::ViewHistory:
      SetView(ui::View::History);
      return true;
    case ui::Action::ViewBranches:
      SetView(ui::View::Branches);
      return true;
    case ui::Action::ViewGraph:
      SetView(ui::View::Graph);
      return true;
    case ui::Action::ViewRemote:
      SetView(ui::View::Remote);
      return true;
    case ui::Action::ViewPipelines:
      SetView(ui::View::Pipelines);
      return true;
    case ui::Action::ViewPulls:
      SetView(ui::View::Pulls);
      return true;

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
      return false;

    case ui::Action::NextRemote:
      NextRemote();
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
      return &row_boxes_;
    case ui::View::Graph:
    case ui::View::Remote:
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
const Event kTick = Event::Special("gittop:tick");

int App::Run() {
  Refresh();

  auto screen = ScreenInteractive::Fullscreen();

  // Every one of these captures the screen by reference, which is why all of
  // them are shut down before it goes out of scope at the end of this function.
  fetcher_.SetNotifier([&screen] { screen.PostEvent(kRemoteReady); });
  pipeline_fetcher_.SetNotifier([&screen] { screen.PostEvent(kPipelinesReady); });
  job_fetcher_.SetNotifier([&screen] { screen.PostEvent(kJobsReady); });
  pull_fetcher_.SetNotifier([&screen] { screen.PostEvent(kPullsReady); });
  transfer_fetcher_.SetNotifier([&screen] { screen.PostEvent(kTransferDone); });
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
    if (confirm_kind_ == ConfirmKind::Push) {
      const std::string remote_name =
          remotes_.empty() ? "the remote" : remotes_[remote_index_].name;
      // Through SafeUrl, not raw: a remote configured with a token in its URL
      // must not print it here any more than it does on the remote panel.
      return ui::ConfirmPane("Push " + snapshot_.branch + " to " + remote_name + "?",
                             remotes_.empty() ? "" : ui::SafeUrl(remotes_[remote_index_].url),
                             /*warning=*/"", "push");
    }
    return ui::ConfirmPane(discard_target_.change == model::Change::Untracked
                               ? "Delete this file?"
                               : "Discard these changes?",
                           discard_target_.path, "This cannot be undone.", "discard");
  });

  // ------------------------------------------------------------ help overlay
  auto help_pane = Renderer([this] { return ui::HelpPane(keys_); });

  // -------------------------------------------------------- transfer overlay
  auto transfer_pane = Renderer([this] { return ui::TransferPane(TransferViewState(), spinner_); });

  auto overlay =
      Container::Tab({commit_pane, confirm_pane, help_pane, transfer_pane}, &overlay_index_);

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

    Elements body{ui::Header(snapshot_), ui::TabBar(view_, &tab_boxes_)};

    // Only the view on screen fills these, so a stale box from another view can
    // never be hit-tested against.
    row_boxes_.clear();

    Element panel;
    switch (view_) {
      case ui::View::Status:
        body.push_back(ui::SummaryRow(snapshot_, bars_, width < 84));
        panel = ui::FileList(snapshot_, selected_, &row_boxes_) | flex;
        break;

      case ui::View::History: {
        // The heatmap costs ten rows. On a short terminal the log is worth
        // more than the graph, so it goes first and the heatmap steps aside.
        if (height >= 30) {
          body.push_back(window(text(" ACTIVITY ") | bold | color(ui::theme().text_dim),
                                ui::ActivityPanel(history_)) |
                         color(ui::theme().border) | bgcolor(ui::theme().surface));
        }
        panel = window(text(" COMMITS ") | bold | color(ui::theme().text_dim),
                       ui::CommitList(history_, commit_selected_, &row_boxes_)) |
                color(ui::theme().border) | bgcolor(ui::theme().surface) | flex;
        break;
      }

      case ui::View::Branches:
        panel = window(text(" BRANCHES ") | bold | color(ui::theme().text_dim),
                       ui::BranchList(history_, branch_selected_, &row_boxes_)) |
                color(ui::theme().border) | bgcolor(ui::theme().surface) | flex;
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
        panel = ui::PipelinePanel(pipelines_, jobs_, remote_.ref, PipelineViewState(), width,
                                  height, spinner_, &row_boxes_);
        break;

      case ui::View::Pulls: {
        ui::PullView pull_view;
        pull_view.selected = pull_selected_;
        pull_view.details_open = pull_details_open_;
        panel = ui::PullPanel(pulls_, remote_.ref, pull_view, width, height, spinner_,
                              &row_boxes_);
        break;
      }
    }

    if (panel) {
      // The panel's own box, used to reject a click that landed on a row whose
      // reflected geometry is outside the frame it was clipped to.
      body.push_back(std::move(panel) | reflect(body_box_));
    }

    body.push_back(ui::Footer(message_, message_is_error_, ToastFade(), view_, keys_));

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
    // One a second while the CI view is open. It repaints the countdown and
    // asks whether the interval is up; the answer is usually no.
    if (event == kTick) {
      MaybeAutoRefresh();
      return true;
    }

    // Overlay routing lives here rather than on the panes themselves.
    // Container::Tab drops events unless it is focused, and the confirm, help
    // and transfer panes are plain Renderers with nothing focusable inside
    // them, so handlers attached to those panes never ran at all.
    if (overlay_open_) {
      switch (overlay_index_) {
        case kHelp:
          if (event.is_character() || event == Event::Escape || event == Event::Return) {
            CloseOverlay();
            return true;
          }
          return false;

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

        case kConfirm:
          if (event == Event::Character('y') || event == Event::Character('Y')) {
            if (confirm_kind_ == ConfirmKind::Push) {
              PerformPush();
            } else {
              PerformDiscard();
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
  return 0;
}

}  // namespace gittop
