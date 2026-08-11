#include <git2.h>

#include <cstddef>
#include <string>

#include "git/internal.hpp"
#include "git/repository.hpp"

// Everything that reads or drives an interrupted operation. git leaves that
// state on disk and expects the next command to notice it; a dashboard that
// does not look shows a pile of conflicted files with no reason for them.

namespace gittop::git {
namespace {

using model::Operation;
using model::OperationState;

Operation FromState(int state) {
  switch (state) {
    case GIT_REPOSITORY_STATE_MERGE:
      return Operation::Merge;
    case GIT_REPOSITORY_STATE_REVERT:
    case GIT_REPOSITORY_STATE_REVERT_SEQUENCE:
      return Operation::Revert;
    case GIT_REPOSITORY_STATE_CHERRYPICK:
    case GIT_REPOSITORY_STATE_CHERRYPICK_SEQUENCE:
      return Operation::CherryPick;
    case GIT_REPOSITORY_STATE_BISECT:
      return Operation::Bisect;
    case GIT_REPOSITORY_STATE_REBASE:
    case GIT_REPOSITORY_STATE_REBASE_INTERACTIVE:
    case GIT_REPOSITORY_STATE_REBASE_MERGE:
      return Operation::Rebase;
    case GIT_REPOSITORY_STATE_APPLY_MAILBOX:
    case GIT_REPOSITORY_STATE_APPLY_MAILBOX_OR_REBASE:
      return Operation::ApplyMailbox;
    default:
      break;
  }
  return Operation::None;
}

struct RebaseHandle {
  git_rebase* rebase = nullptr;
  ~RebaseHandle() {
    if (rebase != nullptr) {
      git_rebase_free(rebase);
    }
  }
};

struct SignatureHandle {
  git_signature* signature = nullptr;
  ~SignatureHandle() {
    if (signature != nullptr) {
      git_signature_free(signature);
    }
  }
};

// refs/heads/topic to topic, refs/remotes/origin/master to origin/master. Both
// prefixes, because a rebase names a branch on one side and a tracking ref on
// the other, and printing one of them in full makes the banner unreadable.
std::string Shorten(const char* ref) {
  const std::string name = ref != nullptr ? ref : "";
  for (const char* prefix : {"refs/heads/", "refs/remotes/"}) {
    const std::size_t n = std::string(prefix).size();
    if (name.size() > n && name.compare(0, n, prefix) == 0) {
      return name.substr(n);
    }
  }
  return name;
}

// Applies operations until the rebase runs out of them or stops at a conflict.
// Shared by starting a rebase and continuing one, because after the first
// commit those two are the same loop and having written it twice is how the
// second copy ends up missing the GIT_EAPPLIED case.
//
// `stopped` comes back true when a conflict interrupted the run, in which case
// nothing is finished and the rebase state stays on disk for the next attempt.
OpResult RunRebase(git_rebase* rebase, git_signature* who, bool* stopped) {
  *stopped = false;
  while (true) {
    git_rebase_operation* operation = nullptr;
    const int rc = git_rebase_next(&operation, rebase);
    if (rc == GIT_ITEROVER) {
      return OpResult::Ok();
    }
    if (rc != 0) {
      return OpResult::Fail(LastError());
    }

    git_oid id{};
    const int committed = git_rebase_commit(&id, rebase, /*author=*/nullptr, who,
                                            /*message_encoding=*/nullptr, /*message=*/nullptr);
    if (committed == GIT_EUNMERGED) {
      *stopped = true;
      return OpResult::Ok();
    }
    // The change this commit carried is already in the new history, so there is
    // nothing to write. git prints "skipped previously applied commit" here and
    // carries on, and treating it as a failure would abort a rebase that is
    // going exactly as it should.
    if (committed == GIT_EAPPLIED) {
      continue;
    }
    if (committed != 0) {
      return OpResult::Fail(LastError());
    }
  }
}

}  // namespace

OperationState Repository::ReadOperation() const {
  OperationState state;
  state.operation = FromState(git_repository_state(const_cast<git_repository*>(repo_.get())));
  if (state.operation == Operation::None) {
    return state;
  }

  if (state.operation != Operation::Rebase) {
    return state;
  }

  // Only a rebase counts itself, and only once its plan has been written to
  // disk. git_rebase_open fails on a state that looks like a rebase but has no
  // readable plan, which is not worth reporting: the banner already knows what
  // operation it is and the step is the part that would be missing.
  RebaseHandle handle;
  if (git_rebase_open(&handle.rebase, const_cast<git_repository*>(repo_.get()), nullptr) != 0) {
    return state;
  }

  state.total = git_rebase_operation_entrycount(handle.rebase);
  const std::size_t current = git_rebase_operation_current(handle.rebase);
  // GIT_REBASE_NO_OPERATION means it has not started the first one yet, which
  // reads as step 1 rather than as step SIZE_MAX.
  state.step = current == GIT_REBASE_NO_OPERATION ? 1 : current + 1;

  const std::string branch = Shorten(git_rebase_orig_head_name(handle.rebase));
  const std::string onto = Shorten(git_rebase_onto_name(handle.rebase));
  if (!branch.empty() && !onto.empty()) {
    state.detail = branch + " onto " + onto;
  }
  return state;
}

OpResult Repository::RebaseOntoUpstream() {
  git_repository* repo = repo_.get();

  if (git_repository_state(repo) != GIT_REPOSITORY_STATE_NONE) {
    return OpResult::Fail("something is already in progress here");
  }
  // Refused rather than stashed. A rebase that quietly moves uncommitted work
  // out of the way is how a tool loses the only copy of something, and the user
  // can stash it themselves in one keystroke now.
  const model::StatusSnapshot status = ReadStatus();
  if (status.staged + status.unstaged + status.conflicted > 0) {
    return OpResult::Fail("commit or stash your changes first");
  }

  git_reference* head = nullptr;
  if (git_repository_head(&head, repo) != 0) {
    return OpResult::Fail(LastError());
  }
  if (git_repository_head_detached(repo) == 1) {
    git_reference_free(head);
    return OpResult::Fail("HEAD is detached — there is no branch to rebase");
  }

  git_reference* upstream = nullptr;
  if (git_branch_upstream(&upstream, head) != 0) {
    git_reference_free(head);
    return OpResult::Fail("this branch has no upstream to rebase onto");
  }

  const std::string branch_name = Shorten(git_reference_name(head));
  const std::string upstream_name = Shorten(git_reference_shorthand(upstream));

  git_annotated_commit* onto = nullptr;
  const int annotated = git_annotated_commit_from_ref(&onto, repo, upstream);
  git_reference_free(upstream);
  git_reference_free(head);
  if (annotated != 0) {
    return OpResult::Fail(LastError());
  }

  SignatureHandle who;
  if (git_signature_default(&who.signature, repo) != 0) {
    git_annotated_commit_free(onto);
    return OpResult::Fail("set user.name and user.email before rebasing");
  }

  git_rebase_options opts;
  git_rebase_options_init(&opts, GIT_REBASE_OPTIONS_VERSION);
  RebaseHandle handle;
  // A null `branch` means HEAD and a null `onto` means the upstream, which is
  // the plain `git rebase @{upstream}` this is meant to be.
  const int rc = git_rebase_init(&handle.rebase, repo, /*branch=*/nullptr, onto,
                                 /*onto=*/nullptr, &opts);
  git_annotated_commit_free(onto);
  if (rc != 0) {
    return OpResult::Fail(LastError());
  }

  if (git_rebase_operation_entrycount(handle.rebase) == 0) {
    // Nothing to replay. Finishing rather than aborting is what moves the
    // branch onto the upstream, which is the fast-forward case.
    if (git_rebase_finish(handle.rebase, who.signature) != 0) {
      return OpResult::Fail(LastError());
    }
    return OpResult::Ok(branch_name + " is already on " + upstream_name);
  }

  bool stopped = false;
  const OpResult ran = RunRebase(handle.rebase, who.signature, &stopped);
  if (!ran.ok) {
    return ran;
  }
  if (stopped) {
    return OpResult::Ok("rebase stopped at a conflict — resolve, stage, then continue");
  }
  if (git_rebase_finish(handle.rebase, who.signature) != 0) {
    return OpResult::Fail(LastError());
  }
  return OpResult::Ok("rebased " + branch_name + " onto " + upstream_name);
}

OpResult Repository::RebaseContinue() {
  git_repository* repo = repo_.get();
  if (FromState(git_repository_state(repo)) != Operation::Rebase) {
    return OpResult::Fail("no rebase in progress");
  }

  RebaseHandle handle;
  if (git_rebase_open(&handle.rebase, repo, nullptr) != 0) {
    return OpResult::Fail(LastError());
  }
  SignatureHandle who;
  if (git_signature_default(&who.signature, repo) != 0) {
    return OpResult::Fail("set user.name and user.email before rebasing");
  }

  // The step the rebase is standing on has to be committed before the next one
  // can start; git_rebase_next would otherwise refuse. GIT_EUNMERGED here means
  // the conflicts that stopped it are still unresolved, and saying so is more
  // use than libgit2's own wording.
  if (git_rebase_operation_current(handle.rebase) != GIT_REBASE_NO_OPERATION) {
    git_oid id{};
    const int rc = git_rebase_commit(&id, handle.rebase, /*author=*/nullptr, who.signature,
                                     nullptr, nullptr);
    if (rc == GIT_EUNMERGED) {
      return OpResult::Fail("resolve the conflicts and stage them first");
    }
    if (rc != 0 && rc != GIT_EAPPLIED) {
      return OpResult::Fail(LastError());
    }
  }

  bool stopped = false;
  const OpResult ran = RunRebase(handle.rebase, who.signature, &stopped);
  if (!ran.ok) {
    return ran;
  }
  if (stopped) {
    return OpResult::Ok("stopped at the next conflict — resolve, stage, then continue");
  }
  if (git_rebase_finish(handle.rebase, who.signature) != 0) {
    return OpResult::Fail(LastError());
  }
  return OpResult::Ok("rebase finished");
}

OpResult Repository::RebaseAbort() {
  git_repository* repo = repo_.get();
  const Operation operation = FromState(git_repository_state(repo));
  if (operation == Operation::None) {
    return OpResult::Fail("nothing in progress to abort");
  }

  if (operation == Operation::Rebase) {
    RebaseHandle handle;
    if (git_rebase_open(&handle.rebase, repo, nullptr) != 0) {
      return OpResult::Fail(LastError());
    }
    if (git_rebase_abort(handle.rebase) != 0) {
      return OpResult::Fail(LastError());
    }
    return OpResult::Ok("rebase aborted — the branch is back where it was");
  }

  // A merge, a revert or a cherry-pick has no abort of its own in libgit2.
  // What `git merge --abort` does is reset hard to HEAD and clear the state
  // files, and that is what this is — which is why it is behind a confirm that
  // says the work in the tree goes with it.
  git_object* head = nullptr;
  if (git_revparse_single(&head, repo, "HEAD") != 0) {
    return OpResult::Fail(LastError());
  }
  const int rc = git_reset(repo, head, GIT_RESET_HARD, nullptr);
  git_object_free(head);
  if (rc != 0) {
    return OpResult::Fail(LastError());
  }
  git_repository_state_cleanup(repo);
  return OpResult::Ok(std::string(model::OperationName(operation)) + " aborted");
}

}  // namespace gittop::git
