#include <git2.h>

#include <cstddef>
#include <string>
#include <utility>

#include "git/internal.hpp"
#include "git/repository.hpp"

namespace gittop::git {
namespace {

// libgit2 writes the stash message in git's own shape:
//
//   WIP on master: 1a2b3c4 the commit that was HEAD
//   On master: a message the user typed
//
// Splitting it means a row can print the branch in one colour and the rest in
// another, and a filter can match either. When it does not have that shape —
// which it will not for a stash written by some other tool — the whole string
// stays in `summary` and `branch` is left empty rather than guessed at.
void SplitMessage(model::Stash& stash) {
  const std::string& text = stash.message;
  const std::size_t colon = text.find(": ");
  if (colon == std::string::npos) {
    stash.summary = text;
    return;
  }

  std::string head = text.substr(0, colon);
  for (const char* prefix : {"WIP on ", "On "}) {
    const std::size_t n = std::string(prefix).size();
    if (head.size() > n && head.compare(0, n, prefix) == 0) {
      stash.branch = head.substr(n);
      stash.summary = text.substr(colon + 2);
      return;
    }
  }
  stash.summary = text;
}

// The callback gets an oid but no repository, and the age of a stash is the
// most useful thing on its row — "three weeks ago" is what tells you it is safe
// to drop — so the repository is carried alongside the list.
struct CollectPayload {
  git_repository* repo = nullptr;
  model::StashList* out = nullptr;
};

int CollectCb(std::size_t index, const char* message, const git_oid* id, void* payload) {
  auto* p = static_cast<CollectPayload*>(payload);

  model::Stash stash;
  stash.index = index;
  stash.message = message != nullptr ? message : "";
  if (id != nullptr) {
    char buffer[GIT_OID_MAX_HEXSIZE + 1] = {};
    git_oid_tostr(buffer, sizeof(buffer), id);
    stash.id = buffer;
    stash.short_id = stash.id.substr(0, 7);

    git_commit* commit = nullptr;
    if (git_commit_lookup(&commit, p->repo, id) == 0) {
      stash.time = git_commit_time(commit);
      git_commit_free(commit);
    }
  }
  SplitMessage(stash);

  p->out->entries.push_back(std::move(stash));
  return 0;
}

// The signature every stash is written under. git uses the repository's own
// configured identity, and so does this: a stash attributed to someone else
// would show up in `git stash list` looking like it came from a different
// person, which it did not.
struct SignatureHandle {
  git_signature* signature = nullptr;
  ~SignatureHandle() {
    if (signature != nullptr) {
      git_signature_free(signature);
    }
  }
};

}  // namespace

model::StashList Repository::ReadStashes() const {
  model::StashList list;
  // git_stash_foreach takes a non-const repository even though it only reads
  // the reflog, which is why this const method has to cast. Nothing here
  // mutates anything, and one thread owns a Repository at a time regardless.
  CollectPayload payload{const_cast<git_repository*>(repo_.get()), &list};
  git_stash_foreach(payload.repo, CollectCb, &payload);
  return list;
}

OpResult Repository::StashSave(const std::string& message, bool include_untracked) {
  SignatureHandle who;
  if (git_signature_default(&who.signature, repo_.get()) != 0) {
    return OpResult::Fail("no user.name or user.email configured — " + LastError());
  }

  unsigned int flags = GIT_STASH_DEFAULT;
  if (include_untracked) {
    flags |= GIT_STASH_INCLUDE_UNTRACKED;
  }

  git_oid out{};
  const int rc = git_stash_save(&out, repo_.get(), who.signature,
                                message.empty() ? nullptr : message.c_str(), flags);
  if (rc == GIT_ENOTFOUND) {
    // libgit2's way of saying the working tree was already clean. Not a
    // failure, and reporting it as one would put a red toast on a no-op.
    return OpResult::Ok("nothing to stash");
  }
  if (rc != 0) {
    return OpResult::Fail("stash failed — " + LastError());
  }
  return OpResult::Ok(include_untracked ? "stashed, including untracked files" : "stashed");
}

OpResult Repository::StashApply(std::size_t index) {
  git_stash_apply_options opts;
  git_stash_apply_options_init(&opts, GIT_STASH_APPLY_OPTIONS_VERSION);
  // SAFE, so an apply that would write over an uncommitted edit stops and says
  // which file is in the way rather than taking it. Same choice pull makes.
  opts.checkout_options.checkout_strategy = GIT_CHECKOUT_SAFE;

  const int rc = git_stash_apply(repo_.get(), index, &opts);
  if (rc == GIT_EMERGECONFLICT) {
    return OpResult::Fail("that stash conflicts with the working tree");
  }
  if (rc != 0) {
    return OpResult::Fail("apply failed — " + LastError());
  }
  return OpResult::Ok("applied stash@{" + std::to_string(index) + "} — it is still in the list");
}

OpResult Repository::StashPop(std::size_t index) {
  git_stash_apply_options opts;
  git_stash_apply_options_init(&opts, GIT_STASH_APPLY_OPTIONS_VERSION);
  opts.checkout_options.checkout_strategy = GIT_CHECKOUT_SAFE;

  // git_stash_pop drops the entry only when the apply succeeded, so a conflict
  // leaves the stash where it was and nothing is lost.
  const int rc = git_stash_pop(repo_.get(), index, &opts);
  if (rc == GIT_EMERGECONFLICT) {
    return OpResult::Fail("that stash conflicts with the working tree — it was kept");
  }
  if (rc != 0) {
    return OpResult::Fail("pop failed — " + LastError());
  }
  return OpResult::Ok("popped stash@{" + std::to_string(index) + "}");
}

OpResult Repository::StashDrop(std::size_t index) {
  if (git_stash_drop(repo_.get(), index) != 0) {
    return OpResult::Fail("drop failed — " + LastError());
  }
  return OpResult::Ok("dropped stash@{" + std::to_string(index) + "}");
}

}  // namespace gittop::git
