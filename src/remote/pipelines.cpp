#include "remote/pipelines.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "remote/api.hpp"

namespace gittop::remote {
namespace {

using model::FetchState;
using model::Job;
using model::JobList;
using model::Pipeline;
using model::PipelineSnapshot;
using model::Provider;
using model::RateLimit;
using model::RemoteRef;
using model::RunStatus;
using json = nlohmann::json;

// GitHub answers with two fields. `status` says whether the run is alive, and
// only once it is "completed" does `conclusion` say how it ended — so a run
// that is still going has a null conclusion, and reading conclusion alone would
// report every in-flight run as unknown.
RunStatus GitHubStatus(const std::string& status, const std::string& conclusion) {
  if (status != "completed") {
    if (status == "in_progress") {
      return RunStatus::Running;
    }
    if (status == "waiting" || status == "action_required") {
      return RunStatus::Manual;
    }
    // queued, requested, pending, and anything a future API version adds: all
    // of them mean the run has not started, which is what Queued says.
    return RunStatus::Queued;
  }

  if (conclusion == "success") {
    return RunStatus::Success;
  }
  if (conclusion == "failure" || conclusion == "timed_out" || conclusion == "startup_failure") {
    return RunStatus::Failed;
  }
  if (conclusion == "cancelled") {
    return RunStatus::Cancelled;
  }
  if (conclusion == "action_required") {
    return RunStatus::Manual;
  }
  // "neutral" is GitHub's "finished without passing or failing", and it greys
  // the dot exactly as it does for skipped. Folding the two keeps the panel
  // honest about what it can actually distinguish.
  if (conclusion == "skipped" || conclusion == "neutral" || conclusion == "stale") {
    return RunStatus::Skipped;
  }
  return RunStatus::Unknown;
}

// GitLab answers with one field and more values, none of which need a second
// field to disambiguate.
RunStatus GitLabStatus(const std::string& status) {
  if (status == "running") {
    return RunStatus::Running;
  }
  if (status == "success") {
    return RunStatus::Success;
  }
  if (status == "failed") {
    return RunStatus::Failed;
  }
  // One 'l'. GitLab spells it the American way and always has.
  if (status == "canceled" || status == "canceling") {
    return RunStatus::Cancelled;
  }
  if (status == "skipped") {
    return RunStatus::Skipped;
  }
  if (status == "manual") {
    return RunStatus::Manual;
  }
  if (status == "created" || status == "waiting_for_resource" || status == "preparing" ||
      status == "pending" || status == "scheduled") {
    return RunStatus::Queued;
  }
  return RunStatus::Unknown;
}

// First line only. A commit message can be a page long and the run list gives
// it one row.
std::string FirstLine(const std::string& text) {
  const std::size_t newline = text.find('\n');
  std::string line = newline == std::string::npos ? text : text.substr(0, newline);
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
    line.pop_back();
  }
  return line;
}

int DeriveDuration(std::int64_t started, std::int64_t finished) {
  if (started <= 0 || finished <= 0 || finished < started) {
    return -1;
  }
  return static_cast<int>(finished - started);
}

Pipeline NormalizeGitHubRun(const json& run) {
  Pipeline p;
  // The id arrives as a number and is kept as text: GitHub run ids have already
  // outgrown 32 bits, and the only thing done with one is put it back in a URL.
  if (run.contains("id") && run["id"].is_number_integer()) {
    p.id = std::to_string(run["id"].get<std::int64_t>());
  }
  p.number = IntField(run, "run_number");
  p.title = StringField(run, "name");
  p.branch = StringField(run, "head_branch");
  p.event = StringField(run, "event");
  p.status = GitHubStatus(StringField(run, "status"), StringField(run, "conclusion"));
  p.commit_sha = StringField(run, "head_sha");
  p.commit_title = FirstLine(NestedString(run, "head_commit", "message"));
  p.actor = NestedString(run, "actor", "login");
  p.web_url = StringField(run, "html_url");

  p.created_at = ParseIso8601(StringField(run, "created_at"));
  p.started_at = ParseIso8601(StringField(run, "run_started_at"));
  if (p.started_at == 0) {
    p.started_at = p.created_at;
  }
  // GitHub has no finished_at on a run. updated_at is the last time anything
  // about it changed, which for a completed run is when it completed.
  if (model::RunFinished(p.status)) {
    p.finished_at = ParseIso8601(StringField(run, "updated_at"));
  }
  p.duration_seconds = DeriveDuration(p.started_at, p.finished_at);
  return p;
}

Pipeline NormalizeGitLabPipeline(const json& pipeline) {
  Pipeline p;
  if (pipeline.contains("id") && pipeline["id"].is_number_integer()) {
    p.id = std::to_string(pipeline["id"].get<std::int64_t>());
  }
  p.number = IntField(pipeline, "iid");
  p.title = StringField(pipeline, "name");
  p.branch = StringField(pipeline, "ref");
  p.event = StringField(pipeline, "source");
  p.status = GitLabStatus(StringField(pipeline, "status"));
  p.commit_sha = StringField(pipeline, "sha");
  p.web_url = StringField(pipeline, "web_url");
  // GitLab's pipeline *list* carries neither the commit title nor who started
  // it; both need a request per pipeline to /pipelines/:id. Left empty rather
  // than paid for, and the panel omits what is empty.

  p.created_at = ParseIso8601(StringField(pipeline, "created_at"));
  p.started_at = p.created_at;
  if (model::RunFinished(p.status)) {
    p.finished_at = ParseIso8601(StringField(pipeline, "updated_at"));
  }
  p.duration_seconds = DeriveDuration(p.started_at, p.finished_at);
  return p;
}

Job NormalizeGitHubJob(const json& job) {
  Job j;
  j.name = StringField(job, "name");
  // GitHub has no stage concept. Left empty rather than invented.
  j.status = GitHubStatus(StringField(job, "status"), StringField(job, "conclusion"));
  j.started_at = ParseIso8601(StringField(job, "started_at"));
  j.finished_at = ParseIso8601(StringField(job, "completed_at"));
  j.duration_seconds = DeriveDuration(j.started_at, j.finished_at);
  j.web_url = StringField(job, "html_url");
  return j;
}

Job NormalizeGitLabJob(const json& job) {
  Job j;
  j.name = StringField(job, "name");
  j.stage = StringField(job, "stage");
  j.status = GitLabStatus(StringField(job, "status"));
  j.started_at = ParseIso8601(StringField(job, "started_at"));
  j.finished_at = ParseIso8601(StringField(job, "finished_at"));
  // GitLab reports duration directly, as a float, and null while the job runs.
  // Its own number beats a derived one: it excludes time spent queued.
  if (job.contains("duration") && job["duration"].is_number()) {
    j.duration_seconds = static_cast<int>(job["duration"].get<double>());
  } else {
    j.duration_seconds = DeriveDuration(j.started_at, j.finished_at);
  }
  j.web_url = StringField(job, "web_url");
  return j;
}

std::string RunsEndpoint(const RemoteRef& ref, const std::string& branch, int limit) {
  const std::string base = ProjectEndpoint(ref);
  const std::string per_page = std::to_string(std::clamp(limit, 1, 100));

  switch (ref.provider) {
    case Provider::GitHub:
      return base + "/actions/runs?per_page=" + per_page +
             (branch.empty() ? "" : "&branch=" + PercentEncode(branch));
    case Provider::GitLab:
      return base + "/pipelines?per_page=" + per_page +
             (branch.empty() ? "" : "&ref=" + PercentEncode(branch));
    case Provider::Unknown:
      break;
  }
  return {};
}

std::string JobsEndpoint(const RemoteRef& ref, const std::string& pipeline_id) {
  const std::string base = ProjectEndpoint(ref);
  switch (ref.provider) {
    case Provider::GitHub:
      return base + "/actions/runs/" + PercentEncode(pipeline_id) + "/jobs?per_page=100";
    case Provider::GitLab:
      return base + "/pipelines/" + PercentEncode(pipeline_id) + "/jobs?per_page=100";
    case Provider::Unknown:
      break;
  }
  return {};
}

// GitHub wraps its lists in an object with a named array; GitLab returns the
// array itself. Both end up here.
const json* ArrayIn(const json& body, const char* github_key) {
  if (body.is_array()) {
    return &body;
  }
  if (body.is_object() && body.contains(github_key) && body[github_key].is_array()) {
    return &body[github_key];
  }
  return nullptr;
}

}  // namespace

PipelineSnapshot FetchPipelines(const RemoteRef& ref, const Token& token,
                                const std::string& branch, int limit, HttpClient& client,
                                const std::atomic<bool>* cancel) {
  PipelineSnapshot snapshot;
  snapshot.branch = branch;
  snapshot.fetched_at = NowSeconds();

  const auto fail = [&snapshot](std::string error, std::string hint) {
    snapshot.state = FetchState::Failed;
    snapshot.error = std::move(error);
    snapshot.hint = std::move(hint);
    return snapshot;
  };

  if (!ref.valid()) {
    return fail("no supported remote",
                "gittop reads GitHub Actions and GitLab CI; name other hosts in your config");
  }

  HttpRequest request;
  request.url = RunsEndpoint(ref, branch, limit);
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
    const char* subject = ref.provider == Provider::GitHub ? "workflow runs" : "pipelines";
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

  const json* runs = ArrayIn(body, "workflow_runs");
  if (runs == nullptr) {
    return fail("the API returned no run list",
                "check the api base for this host in your config file");
  }

  snapshot.runs.reserve(runs->size());
  for (const json& entry : *runs) {
    if (!entry.is_object()) {
      continue;
    }
    Pipeline run = ref.provider == Provider::GitHub ? NormalizeGitHubRun(entry)
                                                    : NormalizeGitLabPipeline(entry);
    if (run.id.empty()) {
      continue;  // nothing can be done with a run that cannot be addressed
    }

    // Counted here rather than in the panel, which would otherwise walk the
    // list four times to draw four numbers.
    switch (run.status) {
      case RunStatus::Running:
        ++snapshot.running;
        break;
      case RunStatus::Failed:
        ++snapshot.failed;
        break;
      case RunStatus::Success:
        ++snapshot.succeeded;
        break;
      case RunStatus::Queued:
      case RunStatus::Manual:
        ++snapshot.pending;
        break;
      case RunStatus::Cancelled:
      case RunStatus::Skipped:
      case RunStatus::Unknown:
        break;
    }
    snapshot.runs.push_back(std::move(run));
  }

  // Newest first. GitHub already sorts this way and GitLab usually does, but
  // "usually" is not something a list ordered by time should rest on.
  std::stable_sort(snapshot.runs.begin(), snapshot.runs.end(),
                   [](const Pipeline& a, const Pipeline& b) {
                     return a.created_at > b.created_at;
                   });

  snapshot.state = FetchState::Ready;
  return snapshot;
}

JobList FetchJobs(const RemoteRef& ref, const Token& token, const std::string& pipeline_id,
                  HttpClient& client, const std::atomic<bool>* cancel) {
  JobList jobs;
  jobs.pipeline_id = pipeline_id;
  jobs.fetched_at = NowSeconds();

  const auto fail = [&jobs](std::string error, std::string hint) {
    jobs.state = FetchState::Failed;
    jobs.error = std::move(error);
    jobs.hint = std::move(hint);
    return jobs;
  };

  if (!ref.valid() || pipeline_id.empty()) {
    return fail("no run to read jobs for", {});
  }

  HttpRequest request;
  request.url = JobsEndpoint(ref, pipeline_id);
  request.headers = HeadersFor(ref, token);

  const HttpResponse response = client.Get(request, cancel);

  if (!response.transport_ok) {
    if (response.error == "cancelled") {
      return fail("cancelled", {});
    }
    return fail(response.error.empty() ? "the request failed" : response.error, {});
  }

  jobs.rate = ReadRateLimit(response, ref.provider);

  if (!response.success()) {
    std::string error;
    std::string hint;
    DescribeStatus(response.status, token, jobs.rate, ref.provider, "jobs", &error, &hint);
    return fail(std::move(error), std::move(hint));
  }

  const json body = json::parse(response.body, nullptr, false);
  if (body.is_discarded()) {
    return fail("the API returned something that is not JSON", {});
  }

  const json* list = ArrayIn(body, "jobs");
  if (list == nullptr) {
    return fail("the API returned no job list", {});
  }

  jobs.jobs.reserve(list->size());
  for (const json& entry : *list) {
    if (!entry.is_object()) {
      continue;
    }
    jobs.jobs.push_back(ref.provider == Provider::GitHub ? NormalizeGitHubJob(entry)
                                                         : NormalizeGitLabJob(entry));
  }

  // GitLab returns jobs newest-first, which puts the last stage at the top and
  // reads backwards. Both providers end up in execution order.
  std::stable_sort(jobs.jobs.begin(), jobs.jobs.end(), [](const Job& a, const Job& b) {
    if (a.started_at != b.started_at) {
      // A job that never started sorts last rather than to the epoch.
      if (a.started_at == 0) {
        return false;
      }
      if (b.started_at == 0) {
        return true;
      }
      return a.started_at < b.started_at;
    }
    return false;
  });

  jobs.state = FetchState::Ready;
  return jobs;
}

}  // namespace gittop::remote
