#pragma once

#include <atomic>
#include <string>
#include <utility>
#include <vector>

namespace gittop::remote {

// Process-wide curl init and shutdown. Construct exactly one, in main(), before
// any worker thread exists: curl_global_init is not thread-safe and calling it
// lazily from the first request is the classic way to get a rare crash.
class HttpLibrary {
 public:
  HttpLibrary();
  ~HttpLibrary();
  HttpLibrary(const HttpLibrary&) = delete;
  HttpLibrary& operator=(const HttpLibrary&) = delete;

  bool ok() const { return ok_; }

 private:
  bool ok_ = false;
};

struct HttpResponse {
  bool transport_ok = false;  // false means the request never got an answer
  long status = 0;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string error;
  int attempts = 0;

  // Case-insensitive, because GitHub sends x-ratelimit-remaining and GitLab
  // sends RateLimit-Remaining and HTTP/2 lowercases everything anyway.
  std::string Header(const std::string& name) const;

  bool success() const { return transport_ok && status >= 200 && status < 300; }
};

enum class HttpMethod { Get, Post };

struct HttpRequest {
  std::string url;
  std::vector<std::string> headers;  // "Name: value", already assembled
  HttpMethod method = HttpMethod::Get;

  // Only read for a Post, and form-encoded by the caller. It can carry a
  // client_id and a device code, so it is never logged and never displayed —
  // the same rule the Authorization header already lives under.
  std::string body;

  int timeout_seconds = 10;
  int connect_timeout_seconds = 5;
  int max_attempts = 3;
};

class HttpClient {
 public:
  HttpClient() = default;

  // Blocking. Retries transport failures, 429, and 5xx with exponential
  // backoff, honouring Retry-After when it is short enough to be worth waiting
  // for; a 4xx other than 429 is an answer, not a hiccup, and returns at once.
  //
  // `cancel` is polled during transfer and between retries so quitting the app
  // does not wait out a ten-second timeout. Pass nullptr when there is nothing
  // to cancel for.
  HttpResponse Send(const HttpRequest& request, const std::atomic<bool>* cancel = nullptr);

  HttpResponse Get(const HttpRequest& request, const std::atomic<bool>* cancel = nullptr);

  // Retries the same body, which is safe only because both endpoints that use
  // this are idempotent by design: RFC 8628 exists to be polled, and asking for
  // a device code twice yields two codes of which one is simply never used.
  // Anything less forgiving does not belong on this retry loop.
  HttpResponse Post(const HttpRequest& request, const std::atomic<bool>* cancel = nullptr);
};

}  // namespace gittop::remote
