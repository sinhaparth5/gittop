#include "remote/client.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

#include "remote/api.hpp"

namespace gittop::remote {
namespace {

using model::FetchState;
using model::Provider;
using model::RateLimit;
using model::RemoteRef;
using model::RemoteSnapshot;
using model::RepoInfo;
using json = nlohmann::json;

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

}  // namespace

model::RemoteSnapshot FetchRepoInfo(const model::RemoteRef& ref, const Token& token,
                                    HttpClient& client, const std::atomic<bool>* cancel) {
  if (!ref.valid()) {
    return Failure(ref, token, "no supported remote",
                   "gittop reads GitHub and GitLab; name others in your config file");
  }

  HttpRequest request;
  request.url = ProjectEndpoint(ref);
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
    DescribeStatus(response.status, token, rate, ref.provider, "repository", &error, &hint);
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
