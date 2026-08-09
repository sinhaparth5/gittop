#pragma once

#include <string>

#include "git/repository.hpp"
#include "model/status.hpp"

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

  git::Repository repo_;
  model::StatusSnapshot snapshot_;

  int selected_ = 0;
  std::string message_;
  bool message_is_error_ = false;

  std::string commit_message_;
  model::StatusEntry discard_target_;

  int overlay_index_ = kCommit;
  bool overlay_open_ = false;
};

}  // namespace gittop
