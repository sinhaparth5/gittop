#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace gittop::remote {

// The async seam the whole design has been built around since Phase 0: the UI
// thread starts a fetch and keeps rendering, and the answer arrives later as an
// FTXUI event rather than as a return value.
//
// Threading contract:
//   - Start() and Consume() are called only from the UI thread.
//   - The notifier fires on the worker thread and must do nothing but wake the
//     screen. ScreenInteractive::PostEvent is safe for exactly that.
//   - Shutdown() must run before the screen it notifies is destroyed. App::Run
//     calls it after Loop() returns, which is what keeps the callback from
//     outliving its capture.
//
// A template since Phase 4, because pipelines and jobs need the same contract
// and duplicating it twice more is how one of the three copies ends up subtly
// different. The task is a plain callable so the knowledge of *what* is being
// fetched stays in the client files; this one only knows how to run something
// off the UI thread and hand the result back on it.
//
// One task at a time per instance. App keeps a separate Fetcher per concern so
// a polling pipeline refresh can never sit in front of a repository fetch the
// user just asked for.
template <typename Result>
class Fetcher {
 public:
  // Receives the cancel flag rather than capturing one, so a task can hand it
  // to HttpClient::Get and be abandoned mid-request.
  using Task = std::function<Result(const std::atomic<bool>&)>;

  Fetcher() = default;
  ~Fetcher() { Shutdown(); }
  Fetcher(const Fetcher&) = delete;
  Fetcher& operator=(const Fetcher&) = delete;

  void SetNotifier(std::function<void()> notifier) {
    std::lock_guard<std::mutex> lock(mutex_);
    notifier_ = std::move(notifier);
  }

  // Ignored while a task is already in flight, so holding `r` down cannot
  // stack up threads.
  void Start(Task task) {
    if (running_.load()) {
      return;
    }
    Join();  // reap the previous worker before spawning another

    cancel_.store(false);
    running_.store(true);

    worker_ = std::thread([this, task = std::move(task)]() mutable {
      Result result = task(cancel_);

      std::function<void()> notifier;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        // A cancelled fetch is being torn down; publishing its result would
        // only race with the shutdown that asked for the cancel.
        if (!cancel_.load()) {
          result_ = std::move(result);
          notifier = notifier_;
        }
      }

      // running_ drops before the notifier fires, so the frame woken by it
      // already sees the fetch as finished rather than still spinning.
      running_.store(false);
      if (notifier) {
        notifier();
      }
    });
  }

  bool Running() const { return running_.load(); }

  // Moves a finished result out, if there is one. Returns false when nothing
  // has landed yet, which is the normal answer on most frames.
  bool Consume(Result* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!result_.has_value()) {
      return false;
    }
    *out = std::move(*result_);
    result_.reset();
    return true;
  }

  // Cancels anything in flight and joins. Idempotent.
  void Shutdown() {
    cancel_.store(true);
    {
      // Dropped before joining: the worker takes this same lock on its way
      // out, and holding it here would deadlock against the join below.
      std::lock_guard<std::mutex> lock(mutex_);
      notifier_ = nullptr;
    }
    Join();
    running_.store(false);
  }

 private:
  void Join() {
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  std::thread worker_;
  std::atomic<bool> running_{false};
  std::atomic<bool> cancel_{false};

  mutable std::mutex mutex_;
  std::optional<Result> result_;
  std::function<void()> notifier_;
};

}  // namespace gittop::remote
