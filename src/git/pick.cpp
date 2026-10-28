#include <git2.h>

#include <cstddef>
#include <string>
#include <vector>

#include "git/internal.hpp"
#include "git/repository.hpp"

// Cherry-pick and revert. libgit2 applies either one to the index and the
// working tree and leaves the commit to the caller — which is what lets a
// conflict stop here with CHERRY_PICK_HEAD or REVERT_HEAD on disk, where the
// operation pane already knows what to do with it.

namespace gittop::git {
namespace {

std::string Short(const git_oid* id) {
  char out[8] = {};
  git_oid_tostr(out, sizeof(out), id);
  return out;
}

// After git_cherrypick or git_revert: commit what it staged, or report that it
// stopped. `stopped` is true when the index has conflicts, and then nothing is
// committed and the state stays on disk for the user to resolve.
OpResult CommitApplied(git_repository* repo, const git_signature* author,
                       const std::string& message, bool* stopped) {
  *stopped = false;
  git_index* index = nullptr;
  if (git_repository_index(&index, repo) != 0) {
    return OpResult::Fail(LastError());
  }
  if (git_index_has_conflicts(index) == 1) {
    git_index_free(index);
    *stopped = true;
    return OpResult::Ok();
  }
  git_oid tree_id{};
  git_tree* tree = nullptr;
  git_object* head = nullptr;
  git_signature* who = nullptr;
  OpResult result = OpResult::Ok();
  git_oid id{};
  if (git_index_write_tree(&tree_id, index) != 0 || git_tree_lookup(&tree, repo, &tree_id) != 0 ||
      git_revparse_single(&head, repo, "HEAD") != 0) {
    result = OpResult::Fail(LastError());
  } else if (git_signature_default(&who, repo) != 0) {
    result = OpResult::Fail("set user.name and user.email before committing");
  } else {
    const auto* parent = reinterpret_cast<const git_commit*>(head);
    if (git_commit_create(&id, repo, "HEAD", author != nullptr ? author : who, who, nullptr,
                          message.c_str(), tree, 1, &parent) != 0) {
      result = OpResult::Fail(LastError());
    } else {
      git_repository_state_cleanup(repo);
    }
  }
  git_signature_free(who);
  git_object_free(head);
  git_tree_free(tree);
  git_index_free(index);
  return result;
}

}  // namespace

const char* Blocked(Repository& self, git_repository* repo) {
  if (git_repository_state(repo) != GIT_REPOSITORY_STATE_NONE) {
    return "finish or abort what is in progress first";
  }
  const model::StatusSnapshot status = self.ReadStatus();
  if (status.staged + status.unstaged + status.conflicted > 0) {
    return "commit or stash your changes first";
  }
  return nullptr;
}

OpResult Repository::CherryPick(const std::vector<std::string>& ids, std::size_t* applied) {
  *applied = 0;
  git_repository* repo = repo_.get();
  if (const char* why = Blocked(*this, repo); why != nullptr) {
    return OpResult::Fail(why);
  }
  for (const std::string& id : ids) {
    git_oid oid{};
    git_commit* commit = nullptr;
    if (git_oid_fromstr(&oid, id.c_str()) != 0 || git_commit_lookup(&commit, repo, &oid) != 0) {
      return OpResult::Fail(LastError());
    }
    // A merge needs a mainline parent named, and guessing one picks a side.
    if (git_commit_parentcount(commit) > 1) {
      git_commit_free(commit);
      return OpResult::Fail(Short(&oid) + " is a merge — gittop does not pick merges");
    }
    git_cherrypick_options opts;
    git_cherrypick_options_init(&opts, GIT_CHERRYPICK_OPTIONS_VERSION);
    bool stopped = false;
    OpResult step = OpResult::Ok();
    if (git_cherrypick(repo, commit, &opts) != 0) {
      step = OpResult::Fail(LastError());
    } else {
      // The original author, as `git cherry-pick` keeps it; the committer is us.
      step = CommitApplied(repo, git_commit_author(commit), git_commit_message(commit), &stopped);
    }
    git_commit_free(commit);
    if (!step.ok) {
      return step;
    }
    if (stopped) {
      return OpResult::Ok("cherry-pick of " + Short(&oid) +
                          " stopped at a conflict — resolve, stage, then commit");
    }
    ++*applied;
  }
  return OpResult::Ok("picked " + std::to_string(*applied) + " commit" +
                      (*applied == 1 ? "" : "s") + " onto HEAD");
}

OpResult Repository::Revert(const std::string& id) {
  git_repository* repo = repo_.get();
  if (const char* why = Blocked(*this, repo); why != nullptr) {
    return OpResult::Fail(why);
  }
  git_oid oid{};
  git_commit* commit = nullptr;
  if (git_oid_fromstr(&oid, id.c_str()) != 0 || git_commit_lookup(&commit, repo, &oid) != 0) {
    return OpResult::Fail(LastError());
  }
  if (git_commit_parentcount(commit) > 1) {
    git_commit_free(commit);
    return OpResult::Fail(Short(&oid) + " is a merge — gittop does not revert merges");
  }
  // git's own wording, so the log reads the same whichever tool wrote it.
  const char* summary = git_commit_summary(commit);
  const std::string message = std::string("Revert \"") + (summary != nullptr ? summary : "") +
                              "\"\n\nThis reverts commit " + git_oid_tostr_s(&oid) + ".\n";
  git_revert_options opts;
  git_revert_options_init(&opts, GIT_REVERT_OPTIONS_VERSION);
  const int rc = git_revert(repo, commit, &opts);
  git_commit_free(commit);
  if (rc != 0) {
    return OpResult::Fail(LastError());
  }
  bool stopped = false;
  const OpResult step = CommitApplied(repo, nullptr, message, &stopped);
  if (!step.ok) {
    return step;
  }
  if (stopped) {
    return OpResult::Ok("revert of " + Short(&oid) +
                        " stopped at a conflict — resolve, stage, then commit");
  }
  return OpResult::Ok("reverted " + Short(&oid));
}

}  // namespace gittop::git
