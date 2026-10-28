#include <git2.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "git/internal.hpp"
#include "git/repository.hpp"

// HEAD's reflog, and the reset that walks back along it. This is what makes a
// wrong amend, reset or rebase recoverable without remembering `git reflog`.

namespace gittop::git {
namespace {

std::string Hex(const git_oid* id) {
  char out[GIT_OID_MAX_HEXSIZE + 1] = {};
  git_oid_tostr(out, sizeof(out), id);
  return out;
}

bool IsRebase(const model::ReflogEntry& entry) {
  return entry.message.rfind("rebase", 0) == 0;
}

// Where a rebase began: libgit2 writes "rebase: checkout <onto>", git writes
// "rebase (start): ..." and "rebase -i (start): ...".
bool IsRebaseStart(const model::ReflogEntry& entry) {
  return entry.message.rfind("rebase: checkout", 0) == 0 ||
         entry.message.find("(start)") != std::string::npos;
}

}  // namespace

std::size_t UndoSteps(const std::vector<model::ReflogEntry>& log) {
  if (log.empty()) {
    return 0;
  }
  if (!IsRebase(log[0])) {
    return 1;
  }
  std::size_t n = 0;
  while (n < log.size() && IsRebase(log[n])) {
    if (IsRebaseStart(log[n++])) {
      break;
    }
  }
  return n;
}

std::vector<model::ReflogEntry> Repository::ReadReflog(std::size_t limit) const {
  std::vector<model::ReflogEntry> out;
  git_reflog* log = nullptr;
  if (git_reflog_read(&log, repo_.get(), "HEAD") != 0) {
    return out;
  }
  const std::size_t count = git_reflog_entrycount(log);
  for (std::size_t i = 0; i < count && i < limit; ++i) {
    const git_reflog_entry* entry = git_reflog_entry_byindex(log, i);
    model::ReflogEntry row;
    row.id = Hex(git_reflog_entry_id_new(entry));
    row.old_id = Hex(git_reflog_entry_id_old(entry));
    row.short_id = row.id.substr(0, 7);
    const char* message = git_reflog_entry_message(entry);
    row.message = message != nullptr ? message : "";
    // Some writers end the message with a newline; it would draw as a blank.
    while (!row.message.empty() && row.message.back() == '\n') {
      row.message.pop_back();
    }
    row.time = git_reflog_entry_committer(entry)->when.time;
    out.push_back(std::move(row));
  }
  git_reflog_free(log);
  return out;
}

OpResult Repository::ResetTo(const std::string& id) {
  git_repository* repo = repo_.get();
  if (const char* why = Blocked(*this, repo); why != nullptr) {
    return OpResult::Fail(why);
  }
  git_oid oid{};
  git_object* target = nullptr;
  if (git_oid_fromstr(&oid, id.c_str()) != 0 ||
      git_object_lookup(&target, repo, &oid, GIT_OBJECT_COMMIT) != 0) {
    return OpResult::Fail(LastError());
  }
  // Not git_reset's hard mode: that checks out with FORCE, which overwrites an
  // untracked file the target happens to have. A SAFE checkout refuses there
  // instead, and the tree is otherwise clean, so the mixed reset after it only
  // has HEAD and the index left to move.
  git_checkout_options opts;
  git_checkout_options_init(&opts, GIT_CHECKOUT_OPTIONS_VERSION);
  opts.checkout_strategy = GIT_CHECKOUT_SAFE;
  OpResult result = OpResult::Ok("reset to " + id.substr(0, 7));
  if (git_checkout_tree(repo, target, &opts) != 0 ||
      git_reset(repo, target, GIT_RESET_MIXED, nullptr) != 0) {
    result = OpResult::Fail(LastError());
  }
  git_object_free(target);
  return result;
}

}  // namespace gittop::git
