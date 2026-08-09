#include "remote/http.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <string>
#include <thread>

namespace gittop::remote {
namespace {

// A repository payload is a few kilobytes. The cap exists so a misconfigured
// api base pointing at something enormous cannot eat the machine's memory.
constexpr std::size_t kMaxBodyBytes = 8u * 1024u * 1024u;

constexpr const char* kUserAgent = "gittop/0.1.0 (+https://github.com/sinhaparth5/gittop)";

std::string Lower(std::string in) {
  std::transform(in.begin(), in.end(), in.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return in;
}

std::string Trim(const std::string& in) {
  std::size_t begin = 0;
  std::size_t end = in.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(in[begin])) != 0) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(in[end - 1])) != 0) {
    --end;
  }
  return in.substr(begin, end - begin);
}

std::size_t WriteBody(char* data, std::size_t size, std::size_t count, void* user) {
  auto* body = static_cast<std::string*>(user);
  const std::size_t bytes = size * count;
  if (body->size() + bytes > kMaxBodyBytes) {
    return 0;  // aborts the transfer, which surfaces as a write error
  }
  body->append(data, bytes);
  return bytes;
}

std::size_t WriteHeader(char* data, std::size_t size, std::size_t count, void* user) {
  auto* headers = static_cast<std::vector<std::pair<std::string, std::string>>*>(user);
  const std::size_t bytes = size * count;
  const std::string line(data, bytes);

  const std::size_t colon = line.find(':');
  if (colon != std::string::npos) {
    headers->emplace_back(Trim(line.substr(0, colon)), Trim(line.substr(colon + 1)));
  }
  return bytes;
}

int OnProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  const auto* cancel = static_cast<const std::atomic<bool>*>(user);
  return (cancel != nullptr && cancel->load()) ? 1 : 0;  // non-zero aborts
}

bool WorthRetrying(const HttpResponse& response) {
  if (!response.transport_ok) {
    return true;
  }
  if (response.status == 429) {
    return true;
  }
  return response.status >= 500 && response.status < 600;
}

// Sleeps in short slices so a cancel lands quickly instead of after the whole
// backoff has elapsed.
bool SleepCancellable(std::chrono::milliseconds total, const std::atomic<bool>* cancel) {
  constexpr auto kSlice = std::chrono::milliseconds(50);
  std::chrono::milliseconds slept{0};
  while (slept < total) {
    if (cancel != nullptr && cancel->load()) {
      return false;
    }
    const auto step = std::min(kSlice, total - slept);
    std::this_thread::sleep_for(step);
    slept += step;
  }
  return true;
}

std::chrono::milliseconds BackoffFor(const HttpResponse& response, int attempt) {
  // Retry-After is the server telling us exactly how long; believe it, but
  // only up to a few seconds. A rate-limit reset an hour out is not something
  // to block a worker thread on.
  const std::string retry_after = response.Header("Retry-After");
  if (!retry_after.empty()) {
    try {
      const int seconds = std::stoi(retry_after);
      if (seconds > 0 && seconds <= 5) {
        return std::chrono::seconds(seconds);
      }
    } catch (const std::exception&) {
      // A Retry-After can also be an HTTP date. Fall through to the backoff.
    }
  }
  return std::chrono::milliseconds(300 * (1 << attempt));
}

}  // namespace

std::string HttpResponse::Header(const std::string& name) const {
  const std::string wanted = Lower(name);
  for (const auto& [key, value] : headers) {
    if (Lower(key) == wanted) {
      return value;
    }
  }
  return {};
}

HttpLibrary::HttpLibrary() {
  ok_ = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}

HttpLibrary::~HttpLibrary() {
  if (ok_) {
    curl_global_cleanup();
  }
}

HttpResponse HttpClient::Get(const HttpRequest& request, const std::atomic<bool>* cancel) {
  HttpResponse response;
  const int attempts = std::max(1, request.max_attempts);

  for (int attempt = 0; attempt < attempts; ++attempt) {
    if (cancel != nullptr && cancel->load()) {
      response.error = "cancelled";
      return response;
    }

    response.body.clear();
    response.headers.clear();
    response.transport_ok = false;
    response.status = 0;
    response.error.clear();
    response.attempts = attempt + 1;

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
      response.error = "could not initialise an HTTP handle";
      return response;
    }

    curl_slist* headers = nullptr;
    for (const std::string& header : request.headers) {
      headers = curl_slist_append(headers, header.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, WriteHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(request.timeout_seconds));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
                     static_cast<long>(request.connect_timeout_seconds));
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    // Without NOSIGNAL, curl's DNS timeout uses SIGALRM, which is not safe from
    // a worker thread and will take the whole process down with it.
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, OnProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<std::atomic<bool>*>(cancel));
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    const CURLcode code = curl_easy_perform(curl);
    if (code == CURLE_OK) {
      response.transport_ok = true;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    } else if (code == CURLE_ABORTED_BY_CALLBACK) {
      response.error = "cancelled";
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      return response;
    } else {
      response.error = curl_easy_strerror(code);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    const bool last = attempt + 1 >= attempts;
    if (!WorthRetrying(response) || last) {
      return response;
    }
    if (!SleepCancellable(BackoffFor(response, attempt), cancel)) {
      response.error = "cancelled";
      return response;
    }
  }

  return response;
}

}  // namespace gittop::remote
