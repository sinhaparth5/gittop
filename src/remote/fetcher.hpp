#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include "model/remote.hpp"
#include "remote/token.hpp"

namespace gittop::remote {

// The async seam the whole design has been built around since Phase 0: the UI
// thread starts a fetch and keeps rendering, and the answer arrives later as an
// FTXUI event rather than as a return value.
//
// Threading contract:
//   - Start() and Consume() are called only from the UI thread.
//   - The notifier fires on the worker thread and must do nothing but wake the
//     screen. ScreenInteractive::PostEvent is safe for exactly that.
//   - Shutdown() must run before the screen it notifies is destroyed. Run()
//     calls it after Loop() returns, which is what keeps the callback from
//     outliving its capture.
class Fetcher {
 public:
  Fetcher() = default;
  ~Fetcher();
  Fetcher(const Fetcher&) = delete;
  Fetcher& operator=(const Fetcher&) = delete;

  void SetNotifier(std::function<void()> notifier);

  // Ignored while a fetch is already in flight, so holding `r` down cannot
  // stack up threads.
  void Start(model::RemoteRef ref, Token token);

  bool Running() const { return running_.load(); }

  // Moves a finished result out, if there is one. Returns false when nothing
  // has landed yet, which is the normal answer on most frames.
  bool Consume(model::RemoteSnapshot* out);

  // Cancels anything in flight and joins. Idempotent.
  void Shutdown();

 private:
  void Join();

  std::thread worker_;
  std::atomic<bool> running_{false};
  std::atomic<bool> cancel_{false};

  mutable std::mutex mutex_;
  std::optional<model::RemoteSnapshot> result_;
  std::function<void()> notifier_;
};

}  // namespace gittop::remote
