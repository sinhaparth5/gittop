#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "model/remote.hpp"

namespace gittop::model {

// The two providers do not agree on what a CI run is called, how many states it
// can be in, or whether "finished badly" is one state or four. This enum is the
// intersection that a person actually reads off a dashboard, and it is designed
// against both APIs rather than against GitHub with GitLab patched in after.
//
// GitHub splits the answer across two fields — `status` while a run is alive and
// `conclusion` once it is not — so nine conclusions collapse into these. GitLab
// answers with one field and eleven values. Both foldings live in
// remote/pipelines.cpp and nowhere else.
enum class RunStatus {
  Unknown,
  Queued,     // created, pending, preparing, scheduled, waiting for a runner
  Running,
  Success,
  Failed,     // failure, timed out, startup failure
  Cancelled,
  Skipped,    // skipped, stale, and GitHub's "neutral", which it also greys out
  Manual,     // blocked on a person: GitLab manual, GitHub action_required
};

inline std::string RunStatusName(RunStatus status) {
  switch (status) {
    case RunStatus::Queued:
      return "queued";
    case RunStatus::Running:
      return "running";
    case RunStatus::Success:
      return "passed";
    case RunStatus::Failed:
      return "failed";
    case RunStatus::Cancelled:
      return "cancelled";
    case RunStatus::Skipped:
      return "skipped";
    case RunStatus::Manual:
      return "manual";
    case RunStatus::Unknown:
      break;
  }
  return "unknown";
}

// True once a run has reached a state it will not leave on its own. Manual is
// deliberately not terminal: it is waiting, just on a person rather than on a
// runner, so a view that polls should keep polling.
inline bool RunFinished(RunStatus status) {
  switch (status) {
    case RunStatus::Success:
    case RunStatus::Failed:
    case RunStatus::Cancelled:
    case RunStatus::Skipped:
      return true;
    case RunStatus::Queued:
    case RunStatus::Running:
    case RunStatus::Manual:
    case RunStatus::Unknown:
      break;
  }
  return false;
}

// One unit of work inside a run. GitHub calls these jobs and has no stage
// concept; GitLab groups them into stages. `stage` is empty on GitHub rather
// than invented, which is the same rule RepoInfo::watchers follows.
struct Job {
  std::string name;
  std::string stage;
  RunStatus status = RunStatus::Unknown;

  std::int64_t started_at = 0;   // unix seconds, 0 when unknown
  std::int64_t finished_at = 0;
  int duration_seconds = -1;  // -1 when the provider did not say and it cannot be derived

  std::string web_url;
};

// A workflow run or a pipeline. Fields no provider reports are left empty or
// negative rather than defaulted, so the panel can omit them instead of
// printing a zero that reads as a fact about the repository.
struct Pipeline {
  // A string because GitHub run ids have already outgrown 32 bits and the only
  // thing gittop does with an id is put it back in a URL.
  std::string id;
  int number = -1;  // GitHub run_number, GitLab iid

  std::string title;   // workflow name, or the pipeline's own name on GitLab
  std::string branch;
  std::string event;   // push, pull_request, merge_request_event, schedule, …
  RunStatus status = RunStatus::Unknown;

  std::string commit_sha;
  std::string commit_title;  // GitHub only; GitLab's pipeline list omits it
  std::string actor;         // GitHub only, for the same reason

  std::int64_t created_at = 0;
  std::int64_t started_at = 0;
  std::int64_t finished_at = 0;
  int duration_seconds = -1;

  std::string web_url;

  std::string short_sha() const {
    return commit_sha.size() >= 7 ? commit_sha.substr(0, 7) : commit_sha;
  }
};

// The runs for one branch, on the same terms as RemoteSnapshot: every failure
// arrives as a state plus a readable error, never as an exception.
struct PipelineSnapshot {
  FetchState state = FetchState::Idle;

  // What was asked for. Empty means every branch, which is what a detached HEAD
  // gets — there is no branch to filter by and hiding everything would be worse.
  std::string branch;
  std::vector<Pipeline> runs;

  RateLimit rate;
  std::string error;
  std::string hint;
  std::int64_t fetched_at = 0;

  // Counts for the summary row, filled during normalization so the panel does
  // not walk the list four times to draw four numbers.
  int running = 0;
  int failed = 0;
  int succeeded = 0;
  int pending = 0;
};

// Jobs for one run, fetched on demand rather than with the list: a page of runs
// would otherwise cost one request per row, which is the fastest way to spend a
// rate limit on data nobody asked to see.
struct JobList {
  FetchState state = FetchState::Idle;
  std::string pipeline_id;
  std::vector<Job> jobs;

  RateLimit rate;
  std::string error;
  std::string hint;
  std::int64_t fetched_at = 0;
};

}  // namespace gittop::model
