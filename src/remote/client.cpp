#include "remote/client.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace gittop::remote {
namespace {

using model::FetchState;
using model::Provider;
using model::RateLimit;
using model::RemoteRef;
using model::RemoteSnapshot;
using model::RepoInfo;
using json = nlohmann::json;

std::int64_t NowSeconds() {
  return static_cast<std::int64_t>(std::time(nullptr));
}

// Percent-encodes everything a project path can contain, slashes included.
// GitLab addresses a project by its namespaced path with the slashes encoded,
// which is the one place a URL is assembled from user data here.
std::string PercentEncode(const std::string& in) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(in.size() * 3);
  for (const unsigned char c : in) {
    const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                            c == '~';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

// Both APIs hand back RFC 3339 timestamps. timegm rather than mktime: these are
// UTC, and interpreting them in local time would shift "last push" by hours.
std::int64_t ParseIso8601(const std::string& text) {
  if (text.empty()) {
    return 0;
  }
  std::tm tm{};
  std::istringstream stream(text);
  stream >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
  if (stream.fail()) {
    return 0;
  }
  return static_cast<std::int64_t>(timegm(&tm));
}

std::string StringField(const json& object, const char* key) {
  if (!object.contains(key) || !object[key].is_string()) {
    return {};
  }
  return object[key].get<std::string>();
}

int IntField(const json& object, const char* key) {
  if (!object.contains(key) || !object[key].is_number_integer()) {
    return -1;
  }
  return object[key].get<int>();
}

bool BoolField(const json& object, const char* key) {
  return object.contains(key) && object[key].is_boolean() && object[key].get<bool>();
}

int HeaderInt(const HttpResponse& response, const char* name) {
  const std::string value = response.Header(name);
  if (value.empty()) {
    return -1;
  }
  try {
    return std::stoi(value);
  } catch (const std::exception&) {
    return -1;
  }
}

// GitHub sends x-ratelimit-*; GitLab sends RateLimit-* on instances that
// enforce one and nothing at all on those that do not. Absent is not zero.
RateLimit ReadRateLimit(const HttpResponse& response, Provider provider) {
  RateLimit rate;
  const char* limit_name = provider == Provider::GitHub ? "x-ratelimit-limit" : "ratelimit-limit";
  const char* remaining_name =
      provider == Provider::GitHub ? "x-ratelimit-remaining" : "ratelimit-remaining";
  const char* reset_name = provider == Provider::GitHub ? "x-ratelimit-reset" : "ratelimit-reset";

  rate.limit = HeaderInt(response, limit_name);
  rate.remaining = HeaderInt(response, remaining_name);
  const int reset = HeaderInt(response, reset_name);
  if (reset > 0) {
    rate.reset = reset;
  }
  rate.known = rate.limit > 0 && rate.remaining >= 0;
  return rate;
}

RemoteSnapshot Failure(const RemoteRef& ref, const Token& token, std::string error,
                       std::string hint) {
  RemoteSnapshot snapshot;
  snapshot.state = FetchState::Failed;
  snapshot.ref = ref;
  snapshot.token_source = token.source;
  snapshot.token_origin = token.origin;
  snapshot.error = std::move(error);
  snapshot.hint = std::move(hint);
  snapshot.fetched_at = NowSeconds();
  return snapshot;
}

std::string EndpointFor(const RemoteRef& ref) {
  switch (ref.provider) {
    case Provider::GitHub:
      return ref.api_base + "/repos/" + PercentEncode(ref.owner) + "/" + PercentEncode(ref.repo);
    case Provider::GitLab:
      // One encoded path segment, subgroups and all.
      return ref.api_base + "/projects/" + PercentEncode(ref.owner + "/" + ref.repo);
    case Provider::Unknown:
      break;
  }
  return {};
}

std::vector<std::string> HeadersFor(const RemoteRef& ref, const Token& token) {
  std::vector<std::string> headers;
  switch (ref.provider) {
    case Provider::GitHub:
      headers.emplace_back("Accept: application/vnd.github+json");
      headers.emplace_back("X-GitHub-Api-Version: 2022-11-28");
      if (token.present()) {
        headers.emplace_back("Authorization: Bearer " + token.value);
      }
      break;
    case Provider::GitLab:
      headers.emplace_back("Accept: application/json");
      if (token.present()) {
        headers.emplace_back("PRIVATE-TOKEN: " + token.value);
      }
      break;
    case Provider::Unknown:
      break;
  }
  return headers;
}

RepoInfo NormalizeGitHub(const json& body) {
  RepoInfo info;
  info.full_name = StringField(body, "full_name");
  info.description = StringField(body, "description");
  info.default_branch = StringField(body, "default_branch");
  info.web_url = StringField(body, "html_url");
  info.visibility = StringField(body, "visibility");
  if (info.visibility.empty()) {
    info.visibility = BoolField(body, "private") ? "private" : "public";
  }
  info.stars = IntField(body, "stargazers_count");
  info.forks = IntField(body, "forks_count");
  info.open_issues = IntField(body, "open_issues_count");
  info.watchers = IntField(body, "subscribers_count");
  info.archived = BoolField(body, "archived");
  info.last_activity = ParseIso8601(StringField(body, "pushed_at"));
  return info;
}

RepoInfo NormalizeGitLab(const json& body) {
  RepoInfo info;
  info.full_name = StringField(body, "path_with_namespace");
  info.description = StringField(body, "description");
  info.default_branch = StringField(body, "default_branch");
  info.web_url = StringField(body, "web_url");
  info.visibility = StringField(body, "visibility");
  info.stars = IntField(body, "star_count");
  info.forks = IntField(body, "forks_count");
  info.open_issues = IntField(body, "open_issues_count");
  // GitLab has no watcher concept. Left at -1 so the panel omits it rather
  // than printing a zero that would read as "nobody is watching".
  info.archived = BoolField(body, "archived");
  info.last_activity = ParseIso8601(StringField(body, "last_activity_at"));
  return info;
}

// Turns a status code into something a person can act on. The distinction that
// matters most is 404 while anonymous, which nearly always means private
// rather than missing.
void DescribeStatus(long status, const Token& token, const RateLimit& rate, Provider provider,
                    std::string* error, std::string* hint) {
  switch (status) {
    case 401:
      *error = "the token was rejected";
      *hint = token.source == model::TokenSource::Environment
                  ? "check " + token.origin + ", or unset it to browse anonymously"
                  : "check the token in your config file";
      return;
    case 403:
      if (rate.known && rate.remaining == 0) {
        *error = "API rate limit reached";
        *hint = token.present() ? "the limit resets shortly"
                                : "an authenticated token raises the limit substantially";
        return;
      }
      *error = "access forbidden";
      *hint = token.present() ? "the token may lack the scope to read this repository"
                              : "this repository needs a token to read";
      return;
    case 404:
      *error = "repository not found";
      *hint = token.present() ? "check the remote URL, and that the token can see it"
                              : "if it is private, a token is needed to read it";
      return;
    case 429:
      *error = "rate limited";
      *hint = "too many requests; try again in a moment";
      return;
    default:
      break;
  }

  if (status >= 500) {
    *error = std::string(provider == Provider::GitHub ? "GitHub" : "GitLab") + " returned " +
             std::to_string(status);
    *hint = "the API is having trouble; `r` retries";
    return;
  }
  *error = "unexpected response " + std::to_string(status);
  *hint = "`r` retries";
}

}  // namespace

model::RemoteSnapshot FetchRepoInfo(const model::RemoteRef& ref, const Token& token,
                                    HttpClient& client, const std::atomic<bool>* cancel) {
  if (!ref.valid()) {
    return Failure(ref, token, "no supported remote",
                   "gittop reads GitHub and GitLab; name others in your config file");
  }

  HttpRequest request;
  request.url = EndpointFor(ref);
  request.headers = HeadersFor(ref, token);

  const HttpResponse response = client.Get(request, cancel);

  if (!response.transport_ok) {
    if (response.error == "cancelled") {
      return Failure(ref, token, "cancelled", {});
    }
    return Failure(ref, token, response.error.empty() ? "the request failed" : response.error,
                   "gittop works fully offline; the other views need no network");
  }

  const RateLimit rate = ReadRateLimit(response, ref.provider);

  if (!response.success()) {
    std::string error;
    std::string hint;
    DescribeStatus(response.status, token, rate, ref.provider, &error, &hint);
    RemoteSnapshot snapshot = Failure(ref, token, std::move(error), std::move(hint));
    snapshot.rate = rate;
    return snapshot;
  }

  const json body = json::parse(response.body, nullptr, false);
  if (body.is_discarded() || !body.is_object()) {
    return Failure(ref, token, "the API returned something that is not a repository",
                   "check the api base for this host in your config file");
  }

  RemoteSnapshot snapshot;
  snapshot.state = FetchState::Ready;
  snapshot.ref = ref;
  snapshot.token_source = token.source;
  snapshot.token_origin = token.origin;
  snapshot.rate = rate;
  snapshot.fetched_at = NowSeconds();
  snapshot.info = ref.provider == Provider::GitHub ? NormalizeGitHub(body) : NormalizeGitLab(body);

  if (snapshot.info.full_name.empty()) {
    snapshot.info.full_name = ref.full_name();
  }
  if (snapshot.info.web_url.empty()) {
    snapshot.info.web_url = ref.web_url;
  }
  return snapshot;
}

}  // namespace gittop::remote
