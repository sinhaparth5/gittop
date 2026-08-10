#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "model/remote.hpp"

namespace gittop::model {

// GitHub calls it a pull request and GitLab a merge request. They agree on the
// idea and on almost nothing else: the branch fields have different names, the
// state vocabulary differs by one value, and each one reports things the other
// does not. This is the shape both fold into, and remote/pulls.cpp is the only
// file that knows which of them replied.
enum class PullState {
  Unknown,
  Open,
  Merged,
  Closed,
};

inline std::string PullStateName(PullState state) {
  switch (state) {
    case PullState::Open:
      return "open";
    case PullState::Merged:
      return "merged";
    case PullState::Closed:
      return "closed";
    case PullState::Unknown:
      break;
  }
  return "unknown";
}

// Whether the branch would go in cleanly. GitHub's *list* endpoint does not say
// — mergeability is computed lazily and only appears on the single-pull
// endpoint — so this is Unknown on GitHub rather than guessed, in the same way
// RepoInfo::watchers is absent on GitLab. Asking per row would be one request
// each, which is the cost the CI view already refuses to pay for job lists.
enum class MergeStatus {
  Unknown,
  Clean,
  Conflict,
  Blocked,   // discussions unresolved, approvals missing, pipeline required
  Checking,  // the provider has not finished working it out
};

inline std::string MergeStatusName(MergeStatus status) {
  switch (status) {
    case MergeStatus::Clean:
      return "mergeable";
    case MergeStatus::Conflict:
      return "conflicts";
    case MergeStatus::Blocked:
      return "blocked";
    case MergeStatus::Checking:
      return "checking";
    case MergeStatus::Unknown:
      break;
  }
  return "";
}

struct PullRequest {
  int number = -1;  // GitHub number, GitLab iid — what a person calls it
  std::string title;

  PullState state = PullState::Unknown;
  bool draft = false;

  std::string author;
  std::string source_branch;
  std::string target_branch;

  std::vector<std::string> labels;
  std::vector<std::string> reviewers;

  MergeStatus merge_status = MergeStatus::Unknown;
  int comments = -1;  // GitLab's user_notes_count; GitHub's list omits it

  std::int64_t created_at = 0;
  std::int64_t updated_at = 0;

  std::string web_url;

  // True when this is the branch checked out right now, which is nearly always
  // the row the user came to look at. Set during normalization because it needs
  // the branch name, not the provider.
  bool from_head = false;
};

// Open pull requests for one repository, on the same terms as every other
// snapshot here: failures arrive as a state plus a readable error.
struct PullSnapshot {
  FetchState state = FetchState::Idle;

  // The checked-out branch at the time of the fetch, used to mark rows and
  // empty on a detached HEAD.
  std::string branch;
  std::vector<PullRequest> pulls;

  RateLimit rate;
  std::string error;
  std::string hint;
  std::int64_t fetched_at = 0;

  // Filled during normalization so the summary row does not walk the list.
  int drafts = 0;
  int conflicted = 0;
};

}  // namespace gittop::model
