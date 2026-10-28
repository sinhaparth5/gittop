#include <git2.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

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

// gittop's own todo for an interactive rebase, one action per operation, in a
// file beside libgit2's state. libgit2 can only replay picks, so the order and
// the drops go into its own cmt.N files and everything else — squash, fixup,
// reword — lives here, where a continue after a conflict can read it back. The
// state directory is removed by finish and abort, so this goes with it.
constexpr const char* kTodoFile = "gittop-todo";

std::filesystem::path StateDir(git_repository* repo) {
  return std::filesystem::path(git_repository_path(repo)) / "rebase-merge";
}

std::vector<std::string> ReadTodo(git_repository* repo) {
  std::vector<std::string> todo;
  std::ifstream in(StateDir(repo) / kTodoFile);
  for (std::string line; std::getline(in, line);) {
    todo.push_back(line);
  }
  return todo;
}

std::string TrimEnd(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
    text.pop_back();
  }
  return text;
}

// Commits the operation the rebase is standing on, as its todo line says.
// git_rebase_commit always writes onto HEAD, so a squash or a fixup moves HEAD
// back to the previous commit's parent first: the index already holds both
// changes, and committing it there is the two commits made one.
int CommitStep(git_repository* repo, git_rebase* rebase, git_signature* who,
               const std::vector<std::string>& todo) {
  git_oid id{};
  const std::size_t at = git_rebase_operation_current(rebase);
  const std::string action = at < todo.size() ? todo[at] : "pick";
  const bool fold = action == "squash" || action == "fixup";
  if (action.rfind("reword ", 0) != 0 && !fold) {
    return git_rebase_commit(&id, rebase, nullptr, who, nullptr, nullptr);
  }

  git_commit* picked = nullptr;
  if (git_commit_lookup(&picked, repo, &git_rebase_operation_byindex(rebase, at)->id) != 0) {
    return -1;
  }
  int rc = 0;
  if (!fold) {
    const char* body = git_commit_body(picked);
    std::string message = action.substr(7);
    if (body != nullptr) {
      message += std::string("\n\n") + body;
    }
    rc = git_rebase_commit(&id, rebase, nullptr, who, nullptr, message.c_str());
    git_commit_free(picked);
    return rc;
  }

  git_index* index = nullptr;
  git_object* head = nullptr;
  git_commit* parent = nullptr;
  if (git_repository_index(&index, repo) == 0 && git_index_has_conflicts(index) == 1) {
    rc = GIT_EUNMERGED;  // nothing moves until the conflict is resolved
  } else if (git_revparse_single(&head, repo, "HEAD") != 0 ||
             git_commit_parent(&parent, reinterpret_cast<git_commit*>(head), 0) != 0) {
    // The commit to fold into is the root, which a rebase cannot rewrite; keep
    // the change as a commit of its own rather than lose it.
    rc = git_rebase_commit(&id, rebase, nullptr, who, nullptr, nullptr);
  } else {
    const auto* into = reinterpret_cast<git_commit*>(head);
    std::string message = git_commit_message(into);
    if (action == "squash") {
      message = TrimEnd(message) + "\n\n" + git_commit_message(picked);
    }
    git_repository_set_head_detached(repo, git_commit_id(parent));
    rc = git_rebase_commit(&id, rebase, git_commit_author(into), who, nullptr, message.c_str());
    if (rc != 0 && rc != GIT_EAPPLIED) {
      git_repository_set_head_detached(repo, git_commit_id(into));
    }
  }
  git_commit_free(parent);
  git_object_free(head);
  git_index_free(index);
  git_commit_free(picked);
  return rc;
}

// Applies operations until the rebase runs out of them or stops at a conflict.
// Shared by starting a rebase and continuing one, because after the first
// commit those two are the same loop and having written it twice is how the
// second copy ends up missing the GIT_EAPPLIED case.
//
// `stopped` comes back true when a conflict interrupted the run, in which case
// nothing is finished and the rebase state stays on disk for the next attempt.
OpResult RunRebase(git_repository* repo, git_rebase* rebase, git_signature* who, bool* stopped) {
  *stopped = false;
  const std::vector<std::string> todo = ReadTodo(repo);
  while (true) {
    git_rebase_operation* operation = nullptr;
    const int rc = git_rebase_next(&operation, rebase);
    if (rc == GIT_ITEROVER) {
      return OpResult::Ok();
    }
    if (rc != 0) {
      return OpResult::Fail(LastError());
    }

    const int committed = CommitStep(repo, rebase, who, todo);
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

// The commits an edit rewrites, oldest first, with what to do to each, and the
// commit they go back onto. Walks HEAD's first parents down to `id`, plus one
// more for the edits that reach the commit below it.
struct Plan {
  std::vector<std::pair<git_oid, std::string>> steps;
  git_oid base{};
  RewriteScope scope;
};

Plan BuildPlan(git_repository* repo, const std::string& id, Rewrite kind,
               const std::string& summary) {
  Plan plan;
  git_oid target{};
  if (git_oid_fromstr(&target, id.c_str()) != 0) {
    plan.scope.error = "no commit selected";
    return plan;
  }
  if (git_repository_head_detached(repo) == 1) {
    plan.scope.error = "HEAD is detached — there is no branch to rewrite";
    return plan;
  }
  const bool reaches_parent = kind == Rewrite::Squash || kind == Rewrite::Fixup ||
                              kind == Rewrite::MoveDown;

  // Newest first while walking; reversed once the base is known.
  std::vector<git_oid> chain;
  git_object* object = nullptr;
  if (git_revparse_single(&object, repo, "HEAD") != 0) {
    plan.scope.error = LastError();
    return plan;
  }
  auto* commit = reinterpret_cast<git_commit*>(object);
  bool found = false;
  while (true) {
    const unsigned parents = git_commit_parentcount(commit);
    if (parents > 1) {
      plan.scope.error = "a merge is in the way — only straight history can be edited";
      break;
    }
    chain.push_back(*git_commit_id(commit));
    const bool last = found || (git_oid_equal(git_commit_id(commit), &target) && !reaches_parent);
    found = found || git_oid_equal(git_commit_id(commit), &target);
    if (parents == 0) {
      plan.scope.error = found ? "the root commit cannot be rewritten"
                               : "that commit is not on this branch's own line of history";
      break;
    }
    git_commit* parent = nullptr;
    if (git_commit_parent(&parent, commit, 0) != 0) {
      plan.scope.error = LastError();
      break;
    }
    git_commit_free(commit);
    commit = parent;
    if (last) {
      plan.base = *git_commit_id(commit);
      break;
    }
  }
  git_commit_free(commit);
  if (!plan.scope.error.empty()) {
    return plan;
  }
  if (kind == Rewrite::MoveUp && chain.size() < 2) {
    plan.scope.error = "that is already the newest commit";
    return plan;
  }

  std::reverse(chain.begin(), chain.end());
  for (const git_oid& oid : chain) {
    plan.steps.emplace_back(oid, "pick");
  }
  // The target is the first step for every edit but the two that reach below
  // it, where it is the second.
  const std::size_t at = reaches_parent ? 1 : 0;
  switch (kind) {
    case Rewrite::Squash:
      plan.steps[at].second = "squash";
      break;
    case Rewrite::Fixup:
      plan.steps[at].second = "fixup";
      break;
    case Rewrite::Reword:
      plan.steps[at].second = "reword " + summary;
      break;
    case Rewrite::Drop:
      plan.steps.erase(plan.steps.begin());
      break;
    case Rewrite::MoveDown:
    case Rewrite::MoveUp:
      std::swap(plan.steps[0], plan.steps[1]);
      break;
  }

  plan.scope.commits = chain.size();
  git_reference* head = nullptr;
  git_reference* upstream = nullptr;
  if (git_repository_head(&head, repo) == 0 && git_branch_upstream(&upstream, head) == 0) {
    const git_oid* up = git_reference_target(upstream);
    for (const git_oid& oid : chain) {
      if (up != nullptr && (git_oid_equal(up, &oid) || git_graph_descendant_of(repo, up, &oid) == 1)) {
        ++plan.scope.pushed;
      }
    }
  }
  git_reference_free(upstream);
  git_reference_free(head);
  return plan;
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
  const OpResult ran = RunRebase(repo, handle.rebase, who.signature, &stopped);
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
    const int rc = CommitStep(repo, handle.rebase, who.signature, ReadTodo(repo));
    if (rc == GIT_EUNMERGED) {
      return OpResult::Fail("resolve the conflicts and stage them first");
    }
    if (rc != 0 && rc != GIT_EAPPLIED) {
      return OpResult::Fail(LastError());
    }
  }

  bool stopped = false;
  const OpResult ran = RunRebase(repo, handle.rebase, who.signature, &stopped);
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

RewriteScope Repository::PreviewRewrite(const std::string& id, Rewrite kind) const {
  return BuildPlan(const_cast<git_repository*>(repo_.get()), id, kind, "").scope;
}

OpResult Repository::RewriteHistory(const std::string& id, Rewrite kind,
                                    const std::string& summary) {
  git_repository* repo = repo_.get();
  if (git_repository_state(repo) != GIT_REPOSITORY_STATE_NONE) {
    return OpResult::Fail("something is already in progress here");
  }
  // Refused rather than stashed, for the reason RebaseOntoUpstream gives.
  const model::StatusSnapshot status = ReadStatus();
  if (status.staged + status.unstaged + status.conflicted > 0) {
    return OpResult::Fail("commit or stash your changes first");
  }
  const Plan plan = BuildPlan(repo, id, kind, summary);
  if (!plan.scope.error.empty()) {
    return OpResult::Fail(plan.scope.error);
  }

  SignatureHandle who;
  if (git_signature_default(&who.signature, repo) != 0) {
    return OpResult::Fail("set user.name and user.email before rebasing");
  }
  git_annotated_commit* base = nullptr;
  if (git_annotated_commit_lookup(&base, repo, &plan.base) != 0) {
    return OpResult::Fail(LastError());
  }
  git_rebase_options opts;
  git_rebase_options_init(&opts, GIT_REBASE_OPTIONS_VERSION);
  RebaseHandle handle;
  const int rc = git_rebase_init(&handle.rebase, repo, nullptr, base, nullptr, &opts);
  git_annotated_commit_free(base);
  if (rc != 0) {
    return OpResult::Fail(LastError());
  }

  // libgit2 has written one cmt.N per commit in its own order and offers no way
  // to hand it another, so the plan replaces those files and the rebase is
  // opened again from them. This is the rebase-merge layout git itself wrote
  // for `rebase -m`, and it is the whole of what git_rebase_open reads back.
  const std::size_t written = git_rebase_operation_entrycount(handle.rebase);
  if (written != plan.scope.commits) {
    git_rebase_abort(handle.rebase);
    return OpResult::Fail("the history moved under the rewrite — reload and try again");
  }
  const std::filesystem::path dir = StateDir(repo);
  std::ofstream(dir / "end") << plan.steps.size() << "\n";
  std::ofstream todo(dir / kTodoFile);
  for (std::size_t i = 0; i < written; ++i) {
    const std::filesystem::path file = dir / ("cmt." + std::to_string(i + 1));
    if (i >= plan.steps.size()) {
      std::filesystem::remove(file);
      continue;
    }
    std::ofstream(file) << git_oid_tostr_s(&plan.steps[i].first) << "\n";
    todo << plan.steps[i].second << "\n";
  }
  todo.close();
  git_rebase_free(handle.rebase);
  handle.rebase = nullptr;
  if (git_rebase_open(&handle.rebase, repo, nullptr) != 0) {
    return OpResult::Fail(LastError());
  }

  bool stopped = false;
  const OpResult ran = RunRebase(repo, handle.rebase, who.signature, &stopped);
  if (!ran.ok) {
    return ran;
  }
  if (stopped) {
    return OpResult::Ok("rebase stopped at a conflict — resolve, stage, then continue");
  }
  if (git_rebase_finish(handle.rebase, who.signature) != 0) {
    return OpResult::Fail(LastError());
  }
  return OpResult::Ok("rewrote " + std::to_string(plan.scope.commits) + " commit" +
                      (plan.scope.commits == 1 ? "" : "s"));
}

}  // namespace gittop::git
