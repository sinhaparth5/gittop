#include "remote/pulls.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "remote/api.hpp"

namespace gittop::remote {
namespace {

using model::FetchState;
using model::MergeStatus;
using model::Provider;
using model::PullRequest;
using model::PullSnapshot;
using model::PullState;
using model::RemoteRef;
using json = nlohmann::json;

// GitHub labels are objects with a name; GitLab labels are plain strings. Both
// arrive here and leave as the same vector.
std::vector<std::string> StringList(const json& parent, const char* key, const char* sub) {
  std::vector<std::string> out;
  if (!parent.contains(key) || !parent[key].is_array()) {
    return out;
  }
  for (const json& entry : parent[key]) {
    if (entry.is_string()) {
      out.push_back(entry.get<std::string>());
    } else if (entry.is_object() && sub != nullptr) {
      std::string value = StringField(entry, sub);
      if (!value.empty()) {
        out.push_back(std::move(value));
      }
    }
  }
  return out;
}

// GitHub reports "open" or "closed" and puts the difference between closed and
// merged in a separate timestamp. Reading `state` alone would file every merged
// pull request under closed, which is the same shape of mistake as reading
// GitHub's CI conclusion without its status.
PullState GitHubState(const json& pull) {
  const std::string state = StringField(pull, "state");
  if (state == "open") {
    return PullState::Open;
  }
  if (state == "closed") {
    return ParseIso8601(StringField(pull, "merged_at")) > 0 ? PullState::Merged
                                                            : PullState::Closed;
  }
  return PullState::Unknown;
}

// GitLab says it in one field, with one extra value: "locked" is an open merge
// request whose discussion is closed to new comments. That is a moderation
// state rather than a merge state, and a dashboard row has nothing useful to do
// with it, so it folds into open.
PullState GitLabState(const std::string& state) {
  if (state == "opened" || state == "locked") {
    return PullState::Open;
  }
  if (state == "merged") {
    return PullState::Merged;
  }
  if (state == "closed") {
    return PullState::Closed;
  }
  return PullState::Unknown;
}

MergeStatus GitLabMergeStatus(const json& mr) {
  if (BoolField(mr, "has_conflicts")) {
    return MergeStatus::Conflict;
  }

  // detailed_merge_status replaced merge_status in GitLab 15.6 and the old
  // field is still sent by older self-hosted instances, which gittop is
  // explicitly built to talk to.
  const std::string detailed = StringField(mr, "detailed_merge_status");
  if (!detailed.empty()) {
    if (detailed == "mergeable") {
      return MergeStatus::Clean;
    }
    if (detailed == "conflict" || detailed == "need_rebase") {
      return MergeStatus::Conflict;
    }
    if (detailed == "checking" || detailed == "unchecked" || detailed == "preparing" ||
        detailed == "approvals_syncing" || detailed == "ci_still_running") {
      return MergeStatus::Checking;
    }
    // not_approved, discussions_not_resolved, ci_must_pass, draft_status,
    // requested_changes, blocked_status and whatever else GitLab adds: all of
    // them mean a person or a check is in the way, which is what Blocked says.
    return MergeStatus::Blocked;
  }

  const std::string legacy = StringField(mr, "merge_status");
  if (legacy == "can_be_merged") {
    return MergeStatus::Clean;
  }
  if (legacy == "cannot_be_merged") {
    return MergeStatus::Conflict;
  }
  if (legacy == "checking" || legacy == "unchecked") {
    return MergeStatus::Checking;
  }
  return MergeStatus::Unknown;
}

PullRequest NormalizeGitHubPull(const json& pull) {
  PullRequest p;
  p.number = IntField(pull, "number");
  p.title = StringField(pull, "title");
  p.state = GitHubState(pull);
  p.draft = BoolField(pull, "draft");
  p.author = NestedString(pull, "user", "login");
  p.source_branch = NestedString(pull, "head", "ref");
  p.target_branch = NestedString(pull, "base", "ref");
  p.labels = StringList(pull, "labels", "name");
  p.reviewers = StringList(pull, "requested_reviewers", "login");
  p.web_url = StringField(pull, "html_url");
  p.created_at = ParseIso8601(StringField(pull, "created_at"));
  p.updated_at = ParseIso8601(StringField(pull, "updated_at"));

  // merge_status and comments stay at their absent values on purpose: the list
  // endpoint reports neither, and a panel that prints "mergeable" because the
  // field defaulted would be stating something nobody checked.
  return p;
}

PullRequest NormalizeGitLabMr(const json& mr) {
  PullRequest p;
  p.number = IntField(mr, "iid");
  p.title = StringField(mr, "title");
  p.state = GitLabState(StringField(mr, "state"));
  // `draft` since GitLab 14; `work_in_progress` on the instances that predate
  // it, which is the same reason detailed_merge_status has a fallback.
  p.draft = BoolField(mr, "draft") || BoolField(mr, "work_in_progress");
  p.author = NestedString(mr, "author", "username");
  p.source_branch = StringField(mr, "source_branch");
  p.target_branch = StringField(mr, "target_branch");
  p.labels = StringList(mr, "labels", "name");
  p.reviewers = StringList(mr, "reviewers", "username");
  p.merge_status = GitLabMergeStatus(mr);
  p.comments = IntField(mr, "user_notes_count");
  p.web_url = StringField(mr, "web_url");
  p.created_at = ParseIso8601(StringField(mr, "created_at"));
  p.updated_at = ParseIso8601(StringField(mr, "updated_at"));
  return p;
}

// ProjectEndpoint already carries the API base; prefixing it again is how this
// first went out as http://hosthttp://host/repos/... .
std::string PullsEndpoint(const RemoteRef& ref, int limit) {
  const std::string base = ProjectEndpoint(ref);
  const std::string per_page = std::to_string(std::clamp(limit, 1, 100));

  // Sorted by last touched rather than by age: an old pull request that moved
  // this morning is the one worth seeing, and neither API defaults to that.
  switch (ref.provider) {
    case Provider::GitHub:
      return base + "/pulls?state=open&sort=updated&direction=desc&per_page=" + per_page;
    case Provider::GitLab:
      return base + "/merge_requests?state=opened&order_by=updated_at&sort=desc&per_page=" +
             per_page;
    case Provider::Unknown:
      break;
  }
  return {};
}

std::string CreateEndpoint(const RemoteRef& ref) {
  const std::string base = ProjectEndpoint(ref);
  switch (ref.provider) {
    case Provider::GitHub:
      return base + "/pulls";
    case Provider::GitLab:
      return base + "/merge_requests";
    case Provider::Unknown:
      break;
  }
  return {};
}

// The four fields under the names this provider knows them by. This function
// and the two Normalize* above it are the whole of what remote/ knows about the
// difference, in both directions.
std::string CreateBody(const RemoteRef& ref, const model::PullDraft& draft) {
  json payload;
  payload["title"] = draft.title;
  switch (ref.provider) {
    case Provider::GitHub:
      payload["head"] = draft.source_branch;
      payload["base"] = draft.target_branch;
      // Sent even when empty, because "no description" is a thing to say. Both
      // providers take an empty string here and neither invents one.
      payload["body"] = draft.body;
      break;
    case Provider::GitLab:
      payload["source_branch"] = draft.source_branch;
      payload["target_branch"] = draft.target_branch;
      payload["description"] = draft.body;
      break;
    case Provider::Unknown:
      return {};
  }
  return payload.dump();
}

// Joins whatever a body has to say into one line, which is more than
// DescribeStatus can do: a 422 is the provider explaining precisely what is
// wrong with the request, and the generic reading of that status code throws
// the explanation away.
//
// The shapes differ and both are ragged. GitHub sends {"message": "Validation
// Failed", "errors": [...]} where an entry is either an object with its own
// message or one with a field and a code. GitLab sends {"message": [...]}, or
// {"message": {"base": [...]}}, or {"error": "..."} — three shapes from one
// API, which is why this reads defensively rather than indexing.
void CollectMessages(const json& node, std::vector<std::string>* out) {
  if (node.is_string()) {
    std::string text = node.get<std::string>();
    if (!text.empty()) {
      out->push_back(std::move(text));
    }
    return;
  }
  if (node.is_array()) {
    for (const json& entry : node) {
      CollectMessages(entry, out);
    }
    return;
  }
  if (!node.is_object()) {
    return;
  }
  // An object with something readable in it says that; one with only a machine
  // code left says the code, since "code: invalid on field base" beats silence.
  if (node.contains("message")) {
    CollectMessages(node["message"], out);
    return;
  }
  const std::string field = StringField(node, "field");
  const std::string code = StringField(node, "code");
  if (!field.empty() && !code.empty()) {
    out->push_back(code + " on " + field);
  }
}

std::string ProviderComplaint(const json& body) {
  if (!body.is_object()) {
    return {};
  }
  std::vector<std::string> messages;
  if (body.contains("errors")) {
    CollectMessages(body["errors"], &messages);
  }
  // Only when the specific list gave nothing: GitHub's top-level message on a
  // 422 is the useless half ("Validation Failed") and its errors array is the
  // half worth printing, while on GitLab the top-level one is all there is.
  if (messages.empty() && body.contains("message")) {
    CollectMessages(body["message"], &messages);
  }
  if (messages.empty() && body.contains("error")) {
    CollectMessages(body["error"], &messages);
  }

  std::string joined;
  for (std::string& message : messages) {
    joined += joined.empty() ? std::move(message) : "; " + std::move(message);
  }
  return joined;
}

}  // namespace

PullSnapshot FetchPulls(const RemoteRef& ref, const Token& token, const std::string& head_branch,
                        int limit, HttpClient& client, const std::atomic<bool>* cancel) {
  PullSnapshot snapshot;
  snapshot.branch = head_branch;
  snapshot.fetched_at = NowSeconds();

  const auto fail = [&snapshot](std::string error, std::string hint) {
    snapshot.state = FetchState::Failed;
    snapshot.error = std::move(error);
    snapshot.hint = std::move(hint);
    return snapshot;
  };

  if (!ref.valid()) {
    return fail("no supported remote",
                "gittop reads GitHub and GitLab; name other hosts in your config");
  }

  HttpRequest request;
  request.url = PullsEndpoint(ref, limit);
  request.headers = HeadersFor(ref, token);

  const HttpResponse response = client.Get(request, cancel);

  if (!response.transport_ok) {
    if (response.error == "cancelled") {
      return fail("cancelled", {});
    }
    return fail(response.error.empty() ? "the request failed" : response.error,
                "gittop works fully offline; the other views need no network");
  }

  snapshot.rate = ReadRateLimit(response, ref.provider);

  if (!response.success()) {
    const char* subject =
        ref.provider == Provider::GitHub ? "pull requests" : "merge requests";
    std::string error;
    std::string hint;
    DescribeStatus(response.status, token, snapshot.rate, ref.provider, subject, &error, &hint);
    return fail(std::move(error), std::move(hint));
  }

  const json body = json::parse(response.body, nullptr, false);
  if (body.is_discarded()) {
    return fail("the API returned something that is not JSON",
                "check the api base for this host in your config file");
  }
  // Both providers answer with a bare array here — unlike the CI endpoints,
  // where GitHub wraps the list in an object.
  if (!body.is_array()) {
    return fail("the API returned no list of pull requests",
                "check the api base for this host in your config file");
  }

  snapshot.pulls.reserve(body.size());
  for (const json& entry : body) {
    if (!entry.is_object()) {
      continue;
    }
    PullRequest pull = ref.provider == Provider::GitHub ? NormalizeGitHubPull(entry)
                                                        : NormalizeGitLabMr(entry);
    if (pull.number < 0) {
      continue;  // nothing can be done with one that cannot be addressed
    }
    pull.from_head = !head_branch.empty() && pull.source_branch == head_branch;

    if (pull.draft) {
      ++snapshot.drafts;
    }
    if (pull.merge_status == MergeStatus::Conflict) {
      ++snapshot.conflicted;
    }
    snapshot.pulls.push_back(std::move(pull));
  }

  // The branch you are standing on goes to the top whatever its timestamp says:
  // it is the reason this view was opened far more often than not. Everything
  // else keeps the API's most-recently-touched order.
  std::stable_sort(snapshot.pulls.begin(), snapshot.pulls.end(),
                   [](const PullRequest& a, const PullRequest& b) {
                     if (a.from_head != b.from_head) {
                       return a.from_head;
                     }
                     return a.updated_at > b.updated_at;
                   });

  snapshot.state = FetchState::Ready;
  return snapshot;
}

model::PullCreated CreatePull(const RemoteRef& ref, const Token& token,
                              const model::PullDraft& draft, HttpClient& client,
                              const std::atomic<bool>* cancel) {
  model::PullCreated created;

  const auto fail = [&created](std::string error, std::string hint) {
    created.state = FetchState::Failed;
    created.error = std::move(error);
    created.hint = std::move(hint);
    return created;
  };

  if (!ref.valid()) {
    return fail("no supported remote",
                "gittop opens pull requests on GitHub and GitLab");
  }
  // Checked here rather than left to the provider, because both answer a
  // missing branch with a 422 whose wording is about a field name.
  if (draft.source_branch.empty() || draft.target_branch.empty()) {
    return fail("both branches have to be named", "say which branch goes into which");
  }
  if (draft.title.empty()) {
    return fail("a pull request needs a title", "nothing else about it is required");
  }
  if (draft.source_branch == draft.target_branch) {
    return fail("a branch cannot be merged into itself",
                "pick a different target, usually the default branch");
  }

  HttpRequest request;
  request.url = CreateEndpoint(ref);
  request.headers = HeadersFor(ref, token);
  request.headers.emplace_back("Content-Type: application/json");
  request.body = CreateBody(ref, draft);
  // See the header: this is the one request here that must not be repeated.
  request.max_attempts = 1;

  const HttpResponse response = client.Post(request, cancel);

  if (!response.transport_ok) {
    if (response.error == "cancelled") {
      return fail("cancelled", {});
    }
    // Deliberately not "it failed": with no retry and no reply, the server may
    // have made it anyway, and a message that rules that out would be a
    // message that is sometimes wrong about something the user cannot undo.
    return fail(response.error.empty() ? "the request never got an answer" : response.error,
                "refresh the list before trying again — it may have been opened");
  }

  created.rate = ReadRateLimit(response, ref.provider);

  const json body = json::parse(response.body, nullptr, false);

  if (!response.success()) {
    const char* subject = ref.provider == Provider::GitHub ? "pull requests" : "merge requests";

    // What the server said, wherever it said anything. This is the whole
    // difference between a create that explains itself and one that reports a
    // status code: 422 is by far the most likely failure here and it is always
    // a sentence about this specific request.
    const std::string complaint = body.is_discarded() ? std::string() : ProviderComplaint(body);
    if (!complaint.empty()) {
      std::string hint = "check the branches and the title";
      if (response.status == 403 || response.status == 401) {
        hint = "the token needs write access to the repository";
      } else if (response.status == 422 || response.status == 409) {
        hint = "the branch may not be pushed yet, or one may already be open for it";
      }
      return fail(complaint, std::move(hint));
    }

    // 403 gets its own answer rather than DescribeStatus's, which is written
    // for reads and would advise a scope that lets you *see* the list. A token
    // that lists pull requests perfectly well and cannot open one is the
    // ordinary way this fails, and saying "read" there sends the user looking
    // in the wrong place.
    if (response.status == 403) {
      return fail("not allowed to open one here",
                  token.present()
                      ? "the token needs write access (repo, or write_repository)"
                      : "opening one needs a token; sign in first");
    }

    std::string error;
    std::string hint;
    DescribeStatus(response.status, token, created.rate, ref.provider, subject, &error, &hint);
    return fail(std::move(error), std::move(hint));
  }

  if (body.is_discarded() || !body.is_object()) {
    // The create landed — this is a 2xx — so this is not a failure to report as
    // one. It is only the echo that could not be read, and the list refresh
    // that follows will show what was actually made.
    created.state = FetchState::Ready;
    return created;
  }

  created.pull = ref.provider == Provider::GitHub ? NormalizeGitHubPull(body)
                                                  : NormalizeGitLabMr(body);
  created.state = FetchState::Ready;
  return created;
}

}  // namespace gittop::remote
