#include <git2.h>

#include <string>

#include "git/internal.hpp"
#include "git/repository.hpp"

// Making, renaming and deleting branches, and making tags. Local refs only:
// anything the remote has to hear about goes through git/transfer.cpp, on a
// worker, like every other push.

namespace gittop::git {
namespace {

std::string Short(const git_oid* id) {
  char out[8] = {};
  git_oid_tostr(out, sizeof(out), id);
  return out;
}

}  // namespace

OpResult Repository::CreateBranch(const std::string& name, const std::string& from) {
  git_repository* repo = repo_.get();
  if (git_reference_is_valid_name(("refs/heads/" + name).c_str()) != 1) {
    return OpResult::Fail("'" + name + "' is not a valid branch name");
  }
  git_object* target = nullptr;
  if (git_revparse_single(&target, repo, from.c_str()) != 0) {
    return OpResult::Fail(LastError());
  }
  git_object* commit = nullptr;
  git_reference* created = nullptr;
  OpResult result = OpResult::Fail("");
  if (git_object_peel(&commit, target, GIT_OBJECT_COMMIT) != 0 ||
      git_branch_create(&created, repo, name.c_str(),
                        reinterpret_cast<git_commit*>(commit), /*force=*/0) != 0) {
    result = OpResult::Fail(LastError());
  } else {
    result = OpResult::Ok("created " + name + " at " + Short(git_object_id(commit)));
  }
  git_reference_free(created);
  git_object_free(commit);
  git_object_free(target);
  return result;
}

OpResult Repository::RenameBranch(const std::string& name, const std::string& to) {
  git_reference* ref = nullptr;
  if (git_branch_lookup(&ref, repo_.get(), name.c_str(), GIT_BRANCH_LOCAL) != 0) {
    return OpResult::Fail(LastError());
  }
  git_reference* moved = nullptr;
  // Not forced: renaming over an existing branch would delete that one.
  const int rc = git_branch_move(&moved, ref, to.c_str(), /*force=*/0);
  git_reference_free(moved);
  git_reference_free(ref);
  if (rc != 0) {
    return OpResult::Fail(LastError());
  }
  return OpResult::Ok("renamed " + name + " to " + to);
}

bool Repository::BranchMerged(const std::string& name) const {
  // `git branch -d` asks the same question: is every commit on it reachable
  // from HEAD, or from its own upstream? Either way deleting it loses nothing.
  git_repository* repo = const_cast<git_repository*>(repo_.get());
  git_reference* ref = nullptr;
  if (git_branch_lookup(&ref, repo, name.c_str(), GIT_BRANCH_LOCAL) != 0) {
    return false;
  }
  const git_oid* tip = git_reference_target(ref);
  bool merged = false;
  git_oid head{};
  if (tip != nullptr && git_reference_name_to_id(&head, repo, "HEAD") == 0) {
    merged = git_oid_equal(&head, tip) || git_graph_descendant_of(repo, &head, tip) == 1;
  }
  git_reference* upstream = nullptr;
  if (!merged && tip != nullptr && git_branch_upstream(&upstream, ref) == 0) {
    const git_oid* up = git_reference_target(upstream);
    merged = up != nullptr &&
             (git_oid_equal(up, tip) || git_graph_descendant_of(repo, up, tip) == 1);
  }
  git_reference_free(upstream);
  git_reference_free(ref);
  return merged;
}

OpResult Repository::DeleteBranch(const std::string& name) {
  git_reference* ref = nullptr;
  if (git_branch_lookup(&ref, repo_.get(), name.c_str(), GIT_BRANCH_LOCAL) != 0) {
    return OpResult::Fail(LastError());
  }
  if (git_branch_is_head(ref) == 1) {
    git_reference_free(ref);
    return OpResult::Fail("that is the branch you are on — check out another first");
  }
  const std::string tip = Short(git_reference_target(ref));
  const int rc = git_branch_delete(ref);
  git_reference_free(ref);
  if (rc != 0) {
    return OpResult::Fail(LastError());
  }
  // The id is the way back: `git branch <name> <id>` undoes this.
  return OpResult::Ok("deleted " + name + " (was " + tip + ")");
}

OpResult Repository::CreateTag(const std::string& id, const std::string& name,
                               const std::string& message) {
  git_repository* repo = repo_.get();
  if (git_reference_is_valid_name(("refs/tags/" + name).c_str()) != 1) {
    return OpResult::Fail("'" + name + "' is not a valid tag name");
  }
  git_object* target = nullptr;
  if (git_revparse_single(&target, repo, id.c_str()) != 0) {
    return OpResult::Fail(LastError());
  }
  git_oid tag{};
  int rc = 0;
  if (message.empty()) {
    rc = git_tag_create_lightweight(&tag, repo, name.c_str(), target, /*force=*/0);
  } else {
    git_signature* who = nullptr;
    if (git_signature_default(&who, repo) != 0) {
      git_object_free(target);
      return OpResult::Fail("set user.name and user.email before tagging");
    }
    rc = git_tag_create(&tag, repo, name.c_str(), target, who, message.c_str(), /*force=*/0);
    git_signature_free(who);
  }
  const std::string at = Short(git_object_id(target));
  git_object_free(target);
  if (rc != 0) {
    return OpResult::Fail(LastError());
  }
  return OpResult::Ok("tagged " + at + " as " + name);
}

}  // namespace gittop::git
