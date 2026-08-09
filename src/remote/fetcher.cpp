#include "remote/fetcher.hpp"

#include <utility>

#include "remote/client.hpp"
#include "remote/http.hpp"

namespace gittop::remote {

Fetcher::~Fetcher() {
  Shutdown();
}

void Fetcher::SetNotifier(std::function<void()> notifier) {
  std::lock_guard<std::mutex> lock(mutex_);
  notifier_ = std::move(notifier);
}

void Fetcher::Start(model::RemoteRef ref, Token token) {
  if (running_.load()) {
    return;
  }
  Join();  // reap the previous worker before spawning another

  cancel_.store(false);
  running_.store(true);

  worker_ = std::thread([this, ref = std::move(ref), token = std::move(token)]() mutable {
    HttpClient client;
    model::RemoteSnapshot snapshot = FetchRepoInfo(ref, token, client, &cancel_);

    std::function<void()> notifier;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      // A cancelled fetch is being torn down; publishing its result would only
      // race with the shutdown that asked for the cancel.
      if (!cancel_.load()) {
        result_ = std::move(snapshot);
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

bool Fetcher::Consume(model::RemoteSnapshot* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!result_.has_value()) {
    return false;
  }
  *out = std::move(*result_);
  result_.reset();
  return true;
}

void Fetcher::Join() {
  if (worker_.joinable()) {
    worker_.join();
  }
}

void Fetcher::Shutdown() {
  cancel_.store(true);
  {
    // Dropped before joining: the worker takes this same lock on its way out,
    // and holding it here would deadlock against the join below.
    std::lock_guard<std::mutex> lock(mutex_);
    notifier_ = nullptr;
  }
  Join();
  running_.store(false);
}

}  // namespace gittop::remote
