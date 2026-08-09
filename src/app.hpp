#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "git/repository.hpp"
#include "model/history.hpp"
#include "model/remote.hpp"
#include "model/status.hpp"
#include "remote/fetcher.hpp"
#include "ui/graph_panel.hpp"
#include "ui/panels.hpp"

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
  };

  // Re-reads the repository into snapshot_. Phase 3 moves the ReadStatus() call
  // onto a worker thread and posts the finished snapshot back through
  // ScreenInteractive::PostEvent; everything else here stays as it is, which is
  // the whole reason the UI reads from a snapshot instead of from libgit2.
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

  model::StatusSnapshot snapshot_;
  model::HistorySnapshot history_;
  bool history_loaded_ = false;

  std::vector<model::RemoteRef> remotes_;
  model::RemoteSnapshot remote_;
  remote::Fetcher fetcher_;
  bool remotes_discovered_ = false;
  int spinner_ = 0;
  float spinner_accum_ = 0.0F;

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

  int overlay_index_ = kCommit;
  bool overlay_open_ = false;
};

}  // namespace gittop
