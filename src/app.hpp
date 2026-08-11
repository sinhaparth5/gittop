#pragma once

#include <chrono>
#include <cstddef>
#include <ftxui/dom/elements.hpp>
#include <memory>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "git/askpass.hpp"
#include "git/repository.hpp"
#include "git/transfer.hpp"
#include "model/diff.hpp"
#include "model/history.hpp"
#include "model/operation.hpp"
#include "model/pipeline.hpp"
#include "model/pull.hpp"
#include "model/remote.hpp"
#include "model/stash.hpp"
#include "model/status.hpp"
#include "remote/fetcher.hpp"
#include "remote/ticker.hpp"
#include "ui/diff_panel.hpp"
#include "ui/graph_panel.hpp"
#include "ui/keymap.hpp"
#include "ui/panels.hpp"
#include "ui/pipeline_panel.hpp"
#include "ui/pull_panel.hpp"
#include "ui/stash_panel.hpp"

namespace gittop {

class App {
 public:
  App(git::Repository repo, config::Config config, std::string config_path);

  int Run();

 private:
  // Tab indices for the modal container. Kept as ints because FTXUI's
  // Container::Tab selects by int.
  enum Overlay : int {
    kCommit = 0,
    kConfirm = 1,
    kHelp = 2,
    kTransfer = 3,
    kPassphrase = 4,
    kFilter = 5,
    kOperation = 6,
  };

  // What a `y` in the confirm overlay is agreeing to. One overlay rather than
  // one per question, because the routing rule is that every key is decided in
  // a single handler and two confirm panes would need two branches of it.
  enum class ConfirmKind {
    Discard,
    Push,
    StashPop,
    StashDrop,
    Rebase,
    OperationContinue,
    OperationAbort,
  };

  // Which transfer is in flight, for the progress pane's title and for deciding
  // what to reload when it lands.
  enum class TransferKind {
    None,
    Fetch,
    Pull,
    Push,
  };

  // Reads the config into the theme and the keymap. Runs before the first frame
  // so nothing is ever drawn in a palette the user replaced.
  void ApplyConfig();

  // Re-reads the repository into snapshot_.
  void Refresh();

  void Apply(const git::OpResult& result);
  void Note(std::string message, bool is_error);

  void Move(int delta);
  void SelectFirst();
  void SelectLast();

  // History is read lazily and cached: opening a repository should not pay for
  // a revwalk nobody asked to see. Committing invalidates it, and so does `r`.
  void SetView(ui::View view);
  void EnsureHistory();
  void Reload();

  // Diffs sit on the same lazy terms as the history: read on the first switch
  // to the view and cached, thrown away by anything that could change one.
  void EnsureDiff();
  void ShowDiff(const git::DiffRequest& request);
  void OpenSelectedDiff();
  void SwitchDiffSource();
  void JumpFile(int delta);

  // Stashes likewise, except that every mutation renumbers the list, so each
  // one re-reads rather than adjusting what it already has.
  void EnsureStashes();
  void SaveStash();
  void RequestStash(ConfirmKind kind);
  void PerformStashPop();
  void PerformStashDrop();
  void ApplyStash();
  const model::Stash* SelectedStash() const;

  // An interrupted rebase, merge or cherry-pick. Read on every status refresh,
  // because the alternative is a screen full of conflicted files with nothing
  // saying where they came from.
  void RequestRebase();
  void PerformRebase();
  void OpenOperation();
  void RequestOperation(ConfirmKind kind);
  void PerformOperationContinue();
  void PerformOperationAbort();

  // Search. One filter for the whole program rather than one per view: it is
  // cleared on every view switch, so a second one would only ever be stale.
  void OpenFilter();
  void CloseFilter(bool keep);
  void RebuildFilter();
  int FilterMatches() const;
  int FilterTotal() const;
  // The visible data for whichever view is on screen. Returns the untouched
  // snapshot when nothing is being filtered, so the common case copies nothing.
  const model::StatusSnapshot& VisibleStatus() const;
  const model::HistorySnapshot& VisibleHistory() const;
  const model::PipelineSnapshot& VisiblePipelines() const;
  const model::PullSnapshot& VisiblePulls() const;
  const model::StashList& VisibleStashes() const;

  int& ActiveSelection();
  int ActiveCount() const;

  int MaxGraphOffset() const;
  void PanGraph(int buckets);
  void SetBucket(ui::Bucket bucket);

  // The remote is read on the same lazy terms as the history, except that it
  // goes to a worker thread: the fetch is the one thing here that can take
  // seconds, and the UI thread is never allowed to wait on it.
  void DiscoverRemotes();
  void EnsureRemote();
  void StartFetch();
  void CollectFetch();

  // A repository can have several remotes and every network view reads exactly
  // one of them. Changing it drops all three caches, because none of them
  // describes the new one.
  void NextRemote();

  // CI runs sit on the same lazy terms as the remote, plus an interval: a
  // pipeline that finishes while you watch is the whole point of the view.
  void EnsurePipelines();
  void StartPipelineFetch();
  void CollectPipelines();
  void StartJobFetch();
  void CollectJobs();
  void ToggleJobs();

  // Pull requests are one request and no interval. Nothing about a review moves
  // second to second, and the drill-down needs no request at all: everything it
  // shows arrived with the list.
  void EnsurePulls();
  void StartPullFetch();
  void CollectPulls();
  void ToggleDetails();

  // How long until the next automatic refresh, and whether there should be one
  // at all. Anonymous GitHub gets sixty requests an hour, so polling on a timer
  // without asking this first is how a view spends a budget the user wanted for
  // something else.
  int RefreshInterval() const;
  bool AutoRefreshAllowed(std::string* reason) const;
  int SecondsToRefresh() const;
  void MaybeAutoRefresh();
  ui::PipelineView PipelineViewState() const;

  // Push, pull and fetch. All three run on a worker with their own repository
  // handle: libgit2 objects are not safe to share, and repo_ belongs to the UI
  // thread for the whole life of the program.
  void StartTransfer(TransferKind kind);
  void RequestPush();
  void PerformPush();
  void CollectTransfer();
  void CancelTransfer();
  ui::TransferView TransferViewState() const;

  // An encrypted ssh key cannot be answered through libgit2: the exec transport
  // spawns the system ssh, and ssh asks its own questions. The only channel in
  // is SSH_ASKPASS, so gittop collects the passphrase in an overlay first and
  // serves it to that child over a socket for as long as the transfer runs.
  void RequestPassphrase(TransferKind kind);
  void SubmitPassphrase();
  void CancelPassphrase();

  const model::StatusEntry* Selected() const;

  void ToggleStage();
  void StageSelected();
  void UnstageSelected();
  void StageEverything();

  void RequestDiscard();
  void PerformDiscard();

  void OpenCommit();
  void PerformCommit();

  void OpenOverlay(Overlay which);
  void CloseOverlay();

  // Everything a key can do, in one place. The routing handler turns an event
  // into an Action through the keymap and then calls this, so a rebound key
  // needs no change here and a new action needs no change there.
  bool Perform(ui::Action action);
  ui::Scope CurrentScope() const;

  // Mouse. FTXUI cannot be asked where a dom node ended up, so every list
  // reflects its rows' boxes during layout and they are matched here.
  bool HandleMouse(const ftxui::Mouse& mouse);
  std::vector<ftxui::Box>* ActiveRowBoxes();

  // Advances the eased bar levels by the wall time since the previous frame,
  // then reports whether anything is still in motion. The renderer asks for
  // another frame only while this is true, so an idle dashboard costs nothing.
  void Tick();
  bool Animating() const;
  ui::StatBars TargetBars() const;
  float ToastFade() const;

  git::Repository repo_;
  config::Config config_;
  std::string config_path_;
  ui::Keymap keys_;
  // Read once in ApplyConfig rather than per frame. The render lambda asks
  // about it on every repaint and the config cannot change under a running
  // program, so a map lookup in the hot path buys nothing.
  bool compact_ = false;

  model::StatusSnapshot snapshot_;
  model::HistorySnapshot history_;
  bool history_loaded_ = false;

  model::OperationState operation_;

  git::DiffRequest diff_request_;
  model::DiffSnapshot diff_;
  bool diff_loaded_ = false;
  int diff_line_ = 0;

  model::StashList stashes_;
  bool stashes_loaded_ = false;
  int stash_selected_ = 0;

  // The live search, and the mirrors it produces. Rebuilt eagerly by
  // RebuildFilter whenever the query or any source changes, rather than lazily
  // on a dirty flag: the flag is one more thing that can be forgotten, and the
  // sources here change on a keystroke or a fetch, never on a frame.
  std::string filter_;
  model::StatusSnapshot filtered_status_;
  model::HistorySnapshot filtered_history_;
  model::PipelineSnapshot filtered_pipelines_;
  model::PullSnapshot filtered_pulls_;
  model::StashList filtered_stashes_;

  std::vector<model::RemoteRef> remotes_;
  std::size_t remote_index_ = 0;
  model::RemoteSnapshot remote_;
  remote::Fetcher<model::RemoteSnapshot> fetcher_;
  bool remotes_discovered_ = false;
  int spinner_ = 0;
  float spinner_accum_ = 0.0F;

  // A fetcher of its own rather than a queue on the shared one: a refresh that
  // fires on a timer must never sit in front of a repository fetch the user
  // just asked for by pressing `r`.
  model::PipelineSnapshot pipelines_;
  model::JobList jobs_;
  remote::Fetcher<model::PipelineSnapshot> pipeline_fetcher_;
  remote::Fetcher<model::JobList> job_fetcher_;
  remote::Ticker ticker_;

  int pipeline_selected_ = 0;
  bool jobs_open_ = false;
  std::chrono::steady_clock::time_point last_pipeline_fetch_{};

  model::PullSnapshot pulls_;
  remote::Fetcher<model::PullSnapshot> pull_fetcher_;
  int pull_selected_ = 0;
  bool pull_details_open_ = false;

  // The same one-task worker the network fetches use. A transfer is not an HTTP
  // request, but the contract it needs is identical, and a second copy of a
  // subtle threading arrangement is worse than a slightly broad name.
  remote::Fetcher<git::TransferResult> transfer_fetcher_;
  // Progress is the one thing that has to be visible before the result lands,
  // so it goes through a lock rather than through the result channel. Held by
  // shared_ptr so the worker never reaches back into App.
  std::shared_ptr<git::ProgressSink> transfer_sink_;
  TransferKind transfer_kind_ = TransferKind::None;
  bool transfer_cancelling_ = false;
  // `q` during a transfer means "stop this and let me out". The exit cannot
  // happen there and then — shutting down joins the worker, and a stalled
  // socket would hang the whole program on the way out — so the intent is
  // recorded and acted on when the cancelled transfer reports back.
  bool quit_after_transfer_ = false;

  // The ssh key passphrase, kept for the life of the process so that a session
  // asks once rather than once per transfer. It is never written anywhere, never
  // rendered — the overlay's Input is in password mode — and never leaves the
  // process except down the askpass socket to a child ssh spawned.
  //
  // Cleared the moment it is shown to be wrong, and also when a transfer
  // succeeds without the askpass helper ever being consulted, which means no
  // passphrase was needed and holding one would be keeping a secret for nothing.
  std::string ssh_passphrase_;
  std::string passphrase_input_;
  // Which transfer the prompt is standing in front of, resumed on submit.
  TransferKind pending_transfer_ = TransferKind::None;
  bool passphrase_rejected_ = false;
  // Alive only while a transfer is in flight; its destructor stops the listener
  // thread and removes the socket.
  std::unique_ptr<git::AskpassServer> askpass_;

  ui::View view_ = ui::View::Status;

  int selected_ = 0;
  int commit_selected_ = 0;
  int branch_selected_ = 0;
  ui::GraphView graph_;
  std::string message_;
  bool message_is_error_ = false;

  ui::StatBars bars_;
  std::chrono::steady_clock::time_point last_frame_{};
  std::chrono::steady_clock::time_point message_at_{};

  std::string commit_message_;
  model::StatusEntry discard_target_;
  ConfirmKind confirm_kind_ = ConfirmKind::Discard;

  // Filled by the panels during layout and read on the next event. Never read
  // before a frame has been drawn, which the event loop guarantees.
  std::vector<ftxui::Box> row_boxes_;
  std::vector<ftxui::Box> tab_boxes_;
  ftxui::Box body_box_;

  int overlay_index_ = kCommit;
  bool overlay_open_ = false;
  // Only used by the one-column help, on a terminal too narrow for two and too
  // short for the whole list. Reset each time the overlay opens.
  int help_scroll_ = 0;
};

}  // namespace gittop
