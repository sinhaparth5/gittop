#pragma once

#include <chrono>
#include <string>

#include "git/repository.hpp"
#include "model/history.hpp"
#include "model/status.hpp"
#include "ui/graph_panel.hpp"
#include "ui/panels.hpp"

namespace gittop {

class App {
 public:
  explicit App(git::Repository repo);

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
  model::StatusSnapshot snapshot_;
  model::HistorySnapshot history_;
  bool history_loaded_ = false;

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
