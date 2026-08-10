#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace gittop::remote {

// Wakes the UI once a second while it is running, and not at all otherwise.
//
// This exists because an idle dashboard requests no frames — that is the point
// of the RequestAnimationFrame arrangement in App::Tick — so nothing would ever
// notice that a refresh interval had elapsed. Driving the interval by asking
// for animation frames instead would mean repainting at sixty hertz for twenty
// seconds to watch a clock, which is the opposite of what the frame budget is
// for.
//
// One tick a second is enough for both jobs it has: deciding when the interval
// is up, and letting the panel count down to it honestly.
//
// It lives here rather than in ui/ because it obeys the fetcher's rule — the
// notifier captures the ScreenInteractive by reference, so Stop() must run
// before that screen is destroyed. App::Run calls it right after Loop returns.
class Ticker {
 public:
  Ticker() = default;
  ~Ticker() { Stop(); }
  Ticker(const Ticker&) = delete;
  Ticker& operator=(const Ticker&) = delete;

  void SetNotifier(std::function<void()> notifier) {
    std::lock_guard<std::mutex> lock(mutex_);
    notifier_ = std::move(notifier);
  }

  // Idempotent: starting an already-running ticker does nothing rather than
  // stacking a second thread onto the same notifier.
  void Start() {
    if (!stopped_.load()) {
      return;
    }
    Join();
    stopped_.store(false);

    worker_ = std::thread([this] {
      std::unique_lock<std::mutex> lock(mutex_);
      while (!stopped_.load()) {
        // Returns true only when the predicate fired, i.e. when Stop() asked
        // us to leave. A plain sleep would keep the app alive for up to a
        // second after quit.
        if (cv_.wait_for(lock, std::chrono::seconds(1), [this] { return stopped_.load(); })) {
          break;
        }
        // Copied and called with the lock dropped: PostEvent takes the
        // screen's own lock, and holding two is how deadlocks are made.
        std::function<void()> notifier = notifier_;
        lock.unlock();
        if (notifier) {
          notifier();
        }
        lock.lock();
      }
    });
  }

  // Idempotent, and joins before returning.
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_.store(true);
    }
    cv_.notify_all();
    Join();
  }

  bool Running() const { return !stopped_.load(); }

 private:
  void Join() {
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  std::thread worker_;
  std::atomic<bool> stopped_{true};

  std::mutex mutex_;
  std::condition_variable cv_;
  std::function<void()> notifier_;
};

}  // namespace gittop::remote
