#include <doctest/doctest.h>

#include "remote/pipelines.hpp"

using gittop::model::RunStatus;
using gittop::remote::GitHubStatus;
using gittop::remote::GitLabStatus;

TEST_CASE("GitHub's status and conclusion fold into one RunStatus") {
  // A run still going has a null conclusion; reading it alone says Unknown.
  CHECK(GitHubStatus("in_progress", "") == RunStatus::Running);
  CHECK(GitHubStatus("queued", "") == RunStatus::Queued);
  CHECK(GitHubStatus("requested", "") == RunStatus::Queued);
  CHECK(GitHubStatus("waiting", "") == RunStatus::Manual);
  CHECK(GitHubStatus("completed", "success") == RunStatus::Success);
  CHECK(GitHubStatus("completed", "failure") == RunStatus::Failed);
  CHECK(GitHubStatus("completed", "timed_out") == RunStatus::Failed);
  CHECK(GitHubStatus("completed", "startup_failure") == RunStatus::Failed);
  CHECK(GitHubStatus("completed", "cancelled") == RunStatus::Cancelled);
  CHECK(GitHubStatus("completed", "action_required") == RunStatus::Manual);
  CHECK(GitHubStatus("completed", "neutral") == RunStatus::Skipped);
  CHECK(GitHubStatus("completed", "skipped") == RunStatus::Skipped);
  CHECK(GitHubStatus("completed", "stale") == RunStatus::Skipped);
  CHECK(GitHubStatus("completed", "something_new") == RunStatus::Unknown);
}

TEST_CASE("GitLab's one field folds into the same RunStatus") {
  CHECK(GitLabStatus("running") == RunStatus::Running);
  CHECK(GitLabStatus("success") == RunStatus::Success);
  CHECK(GitLabStatus("failed") == RunStatus::Failed);
  CHECK(GitLabStatus("canceled") == RunStatus::Cancelled);
  CHECK(GitLabStatus("canceling") == RunStatus::Cancelled);
  CHECK(GitLabStatus("skipped") == RunStatus::Skipped);
  CHECK(GitLabStatus("manual") == RunStatus::Manual);
  for (const char* queued :
       {"created", "waiting_for_resource", "preparing", "pending", "scheduled"}) {
    CAPTURE(queued);
    CHECK(GitLabStatus(queued) == RunStatus::Queued);
  }
  CHECK(GitLabStatus("cancelled") == RunStatus::Unknown);  // GitHub's spelling, not GitLab's
}
