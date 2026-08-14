#include "git/repository.hpp"

#include <git2.h>

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "git/graph.hpp"
#include "git/internal.hpp"

namespace gittop::git {

// Outside the anonymous namespace because git/diff.cpp, git/stash.cpp and
// git/rebase.cpp define Repository methods of their own and every one of them
// ends a failure path here. See git/internal.hpp.
std::string LastError() {
  const git_error* e = git_error_last();
  if (e != nullptr && e->message != nullptr) {
    return e->message;
  }
  return "unknown libgit2 error";
}

namespace {

using model::Change;
using model::Stage;
using model::StatusEntry;

// libgit2 returns the workdir with a trailing separator, which makes
// std::filesystem::path::filename() come back empty.
std::string BaseName(std::string path) {
  while (path.size() > 1 && path.back() == '/') {
    path.pop_back();
  }
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string PathOf(const git_status_entry* e) {
  if (e->index_to_workdir != nullptr && e->index_to_workdir->new_file.path != nullptr) {
    return e->index_to_workdir->new_file.path;
  }
  if (e->head_to_index != nullptr && e->head_to_index->new_file.path != nullptr) {
    return e->head_to_index->new_file.path;
  }
  return {};
}

std::string OidToString(const git_oid* oid) {
  if (oid == nullptr) {
    return {};
  }
  char buffer[GIT_OID_MAX_HEXSIZE + 1] = {};
  git_oid_tostr(buffer, sizeof(buffer), oid);
  return buffer;
}

constexpr std::int64_t kSecondsPerDay = 86400;

// Frees the index even when an operation bails out halfway through.
struct IndexHandle {
  git_index* index = nullptr;
  ~IndexHandle() {
    if (index != nullptr) {
      git_index_free(index);
    }
  }
};

// The parents of a commit being written, owned so that a failure anywhere
// between looking them up and creating the commit still frees them.
struct ParentList {
  std::vector<git_commit*> commits;
  ~ParentList() {
    for (git_commit* commit : commits) {
      git_commit_free(commit);
    }
  }
};

struct MergeHeadPayload {
  git_repository* repo = nullptr;
  ParentList* parents = nullptr;
};

int MergeHeadCb(const git_oid* oid, void* payload) {
  auto* p = static_cast<MergeHeadPayload*>(payload);
  git_commit* commit = nullptr;
  if (git_commit_lookup(&commit, p->repo, oid) == 0) {
    p->parents->commits.push_back(commit);
  }
  return 0;
}

// A quarter of weekly commit counts for one branch, for the sparkline beside it.
//
// This is a revwalk per branch, which sounds expensive and is not: GIT_SORT_TIME
// yields newest first, so the walk stops at the first commit older than the
// window rather than traversing the whole history behind it. On a branch that
// has been quiet for a year that is one commit. The cap is there for the case
// GIT_SORT_TIME cannot help with — a repository whose commit timestamps are out
// of order, where "older than the window" is not a reason to believe the rest
// are too.
void ReadVelocity(git_repository* repo, const git_oid* tip, std::int64_t today,
                  model::Branch* branch) {
  constexpr std::size_t kMaxPerBranch = 4000;

  const auto weeks = static_cast<std::int64_t>(model::kVelocityWeeks);
  // Aligned to the week HEAD's own arithmetic uses: epoch day 0 was a Thursday,
  // so +3 puts Monday at zero and every bucket starts on a Monday.
  const std::int64_t this_week = today - ((((today + 3) % 7) + 7) % 7);
  const std::int64_t first_week = this_week - ((weeks - 1) * 7);

  git_revwalk* walk = nullptr;
  if (git_revwalk_new(&walk, repo) != 0) {
    return;
  }
  git_revwalk_sorting(walk, GIT_SORT_TIME);
  if (git_revwalk_push(walk, tip) != 0) {
    git_revwalk_free(walk);
    return;
  }

  branch->velocity_known = true;

  git_oid oid;
  std::size_t seen = 0;
  while (seen < kMaxPerBranch && git_revwalk_next(&oid, walk) == 0) {
    ++seen;
    git_commit* commit = nullptr;
    if (git_commit_lookup(&commit, repo, &oid) != 0) {
      continue;
    }
    const std::int64_t day = static_cast<std::int64_t>(git_commit_time(commit)) / kSecondsPerDay;
    git_commit_free(commit);

    if (day < first_week) {
      break;
    }
    const std::int64_t week = day - ((((day + 3) % 7) + 7) % 7);
    const std::int64_t bucket = (week - first_week) / 7;
    if (bucket >= 0 && bucket < weeks) {
      branch->velocity[static_cast<std::size_t>(bucket)]++;
    }
  }
  git_revwalk_free(walk);

  branch->velocity_max =
      *std::max_element(branch->velocity.begin(), branch->velocity.end());
}

// The upstream a branch asks for in its config, whether or not it still exists.
//
// `git_branch_upstream` resolves the tracking ref, so it fails identically for
// a branch that never had an upstream and for one whose upstream was deleted on
// the server and pruned here. `git_branch_upstream_name` reads
// `branch.<name>.remote` and `.merge` and runs them through the remote's
// refspec — config only, no ref lookup — and that gap is the whole signal.
// Without it the second branch is drawn as though nobody ever pushed it.
//
// Returns the short form ("origin/topic"), or empty when there really is no
// upstream configured.
std::string ConfiguredUpstream(git_repository* repo, git_reference* ref) {
  git_buf full = GIT_BUF_INIT;
  if (git_branch_upstream_name(&full, repo, git_reference_name(ref)) != 0) {
    return {};
  }
  std::string name = full.ptr != nullptr ? std::string(full.ptr, full.size) : std::string();
  git_buf_dispose(&full);

  // "refs/heads/" as well, because `branch.<name>.remote = .` tracks a branch in
  // this same repository and never goes through a refspec.
  for (const std::string& prefix : {std::string("refs/remotes/"), std::string("refs/heads/")}) {
    if (name.rfind(prefix, 0) == 0) {
      name.erase(0, prefix.size());
      break;
    }
  }
  return name;
}

// Ahead/behind against the branch's own upstream. `git_graph_ahead_behind`
// walks only as far as the merge base, so on a branch that tracks closely this
// is a handful of objects; it is not the revwalk the lazy-read rule is about.
// A detached or unborn head has no upstream and leaves the fields at zero,
// which the panel draws as absent rather than as "in sync".
void ReadTracking(git_repository* repo, git_reference* head, model::StatusSnapshot* snap) {
  if (git_reference_is_branch(head) != 1) {
    return;
  }
  git_reference* upstream = nullptr;
  if (git_branch_upstream(&upstream, head) != 0) {
    snap->upstream = ConfiguredUpstream(repo, head);
    snap->upstream_gone = !snap->upstream.empty();
    return;
  }
  const char* name = nullptr;
  if (git_branch_name(&name, upstream) == 0 && name != nullptr) {
    snap->upstream = name;
    snap->has_upstream = true;
  }

  const git_oid* local = git_reference_target(head);
  const git_oid* remote = git_reference_target(upstream);
  std::size_t ahead = 0;
  std::size_t behind = 0;
  if (local != nullptr && remote != nullptr &&
      git_graph_ahead_behind(&ahead, &behind, repo, local, remote) == 0) {
    snap->ahead = ahead;
    snap->behind = behind;
  }
  git_reference_free(upstream);
}

// The newest few commits, and only those: the walk is abandoned the moment the
// vector is full, so this costs five commit lookups rather than one per commit
// in the repository. Reusing ReadHistory here would have been the obvious move
// and is exactly the thing that would put a full revwalk on startup.
void ReadRecent(git_repository* repo, git_reference* head, model::StatusSnapshot* snap) {
  const git_oid* tip = git_reference_target(head);
  if (tip == nullptr) {
    return;
  }
  git_revwalk* walk = nullptr;
  if (git_revwalk_new(&walk, repo) != 0) {
    return;
  }
  git_revwalk_sorting(walk, GIT_SORT_TIME);
  if (git_revwalk_push(walk, tip) != 0) {
    git_revwalk_free(walk);
    return;
  }

  snap->recent.reserve(model::kRecentCommits);
  git_oid oid;
  while (snap->recent.size() < model::kRecentCommits && git_revwalk_next(&oid, walk) == 0) {
    git_commit* commit = nullptr;
    if (git_commit_lookup(&commit, repo, &oid) != 0) {
      continue;
    }
    model::RecentCommit entry;
    char short_id[8] = {};
    git_oid_tostr(short_id, sizeof(short_id), &oid);
    entry.short_id = short_id;

    const char* summary = git_commit_summary(commit);
    entry.summary = summary != nullptr ? summary : "";
    const git_signature* author = git_commit_author(commit);
    entry.author = author != nullptr && author->name != nullptr ? author->name : "";
    entry.time = static_cast<std::int64_t>(git_commit_time(commit));

    snap->recent.push_back(std::move(entry));
    git_commit_free(commit);
  }
  git_revwalk_free(walk);
}

}  // namespace

Library::Library() {
  git_libgit2_init();

  // Phase 5 gave libgit2 a network to talk to, and with it the way an idle
  // socket can hang a worker thread forever. That matters more here than in
  // most programs: the cancel flag a transfer checks is only read from libgit2's
  // progress callbacks, and a server that accepts a connection and then says
  // nothing never fires one — so neither `esc` nor `q` could get out, because
  // quitting joins the worker. These are the same budget libcurl already runs
  // with for the REST calls, and they are the only thing that bounds that wait.
  constexpr int kConnectTimeoutMs = 5000;
  constexpr int kIdleTimeoutMs = 10000;
  git_libgit2_opts(GIT_OPT_SET_SERVER_CONNECT_TIMEOUT, kConnectTimeoutMs);
  git_libgit2_opts(GIT_OPT_SET_SERVER_TIMEOUT, kIdleTimeoutMs);
}

Library::~Library() {
  git_libgit2_shutdown();
}

void Repository::Deleter::operator()(git_repository* r) const {
  git_repository_free(r);
}

Repository::Repository(git_repository* repo) : repo_(repo) {}
Repository::Repository(Repository&&) noexcept = default;
Repository& Repository::operator=(Repository&&) noexcept = default;
Repository::~Repository() = default;

std::optional<Repository> Repository::Discover(const std::string& start_path, std::string* error) {
  git_repository* raw = nullptr;
  const int rc =
      git_repository_open_ext(&raw, start_path.c_str(), GIT_REPOSITORY_OPEN_CROSS_FS, nullptr);

  if (rc == GIT_ENOTFOUND) {
    *error = "not a git repository (or any parent): " + start_path;
    return std::nullopt;
  }
  if (rc != 0) {
    *error = LastError();
    return std::nullopt;
  }
  if (git_repository_is_bare(raw) != 0) {
    git_repository_free(raw);
    *error = "bare repository has no working tree to show";
    return std::nullopt;
  }
  return Repository(raw);
}

std::string Repository::WorkdirPath() const {
  const char* wd = git_repository_workdir(repo_.get());
  return wd != nullptr ? std::string(wd) : std::string{};
}

std::vector<model::RefEntry> Repository::ReadRefs() const {
  std::vector<model::RefEntry> refs;

  git_strarray names{};
  if (git_reference_list(&names, repo_.get()) != 0) {
    return refs;
  }

  // Sorted by the target's commit time, newest first, but only for tags. A
  // branch list reads alphabetically because that is how people look a branch
  // up; a tag list read that way puts v1.0 above v2026.08.3, and on a repository
  // whose CI only ever runs on tags the newest one is the whole reason to open
  // this list.
  std::vector<std::pair<std::int64_t, model::RefEntry>> tags;
  std::vector<model::RefEntry> branches;

  const std::string heads = "refs/heads/";
  const std::string remotes = "refs/remotes/";
  const std::string tag_prefix = "refs/tags/";

  for (std::size_t i = 0; i < names.count; ++i) {
    const char* raw = names.strings[i];
    if (raw == nullptr) {
      continue;
    }
    const std::string full = raw;

    if (full.rfind(heads, 0) == 0) {
      branches.push_back({full.substr(heads.size()), false});
      continue;
    }

    if (full.rfind(remotes, 0) == 0) {
      // The remote's own name is not part of what a provider matches: a run on
      // GitHub records `master`, never `origin/master`. Splitting on the first
      // separator rather than against the configured remote names keeps this a
      // string operation, which is all it needs to be.
      const std::string rest = full.substr(remotes.size());
      const std::size_t slash = rest.find('/');
      if (slash == std::string::npos) {
        continue;
      }
      const std::string name = rest.substr(slash + 1);
      // `origin/HEAD` is a symbolic ref naming the default branch, not a branch
      // of its own, and asking a provider for runs on a branch called HEAD gets
      // an empty list rather than an error.
      if (name.empty() || name == "HEAD") {
        continue;
      }
      branches.push_back({name, false});
      continue;
    }

    if (full.rfind(tag_prefix, 0) == 0) {
      std::int64_t when = 0;
      git_reference* ref = nullptr;
      if (git_reference_lookup(&ref, repo_.get(), full.c_str()) == 0) {
        git_object* commit = nullptr;
        // Peel rather than read the ref's own oid: an annotated tag's ref points
        // at the tag object, which has no commit time on it.
        if (git_reference_peel(&commit, ref, GIT_OBJECT_COMMIT) == 0) {
          when = git_commit_time(reinterpret_cast<git_commit*>(commit));
          git_object_free(commit);
        }
        git_reference_free(ref);
      }
      tags.push_back({when, {full.substr(tag_prefix.size()), true}});
    }
  }

  git_strarray_dispose(&names);

  std::sort(branches.begin(), branches.end(),
            [](const model::RefEntry& a, const model::RefEntry& b) { return a.name < b.name; });
  std::stable_sort(tags.begin(), tags.end(),
                   [](const auto& a, const auto& b) { return a.first > b.first; });

  // A local branch and its remote-tracking counterpart are one name to a
  // provider, so they are one row here. A tag sharing a name with a branch is
  // one row too, and it is the branch that survives: the request they would
  // both produce is identical, and two rows that send the same query and differ
  // only in a label is a list that looks broken.
  std::unordered_set<std::string> seen;
  const auto take = [&refs, &seen](const model::RefEntry& entry) {
    if (!seen.insert(entry.name).second) {
      return;
    }
    refs.push_back(entry);
  };

  for (const model::RefEntry& branch : branches) {
    take(branch);
  }
  for (const auto& tag : tags) {
    take(tag.second);
  }
  return refs;
}

std::vector<std::pair<std::string, std::string>> Repository::ReadRemotes() const {
  std::vector<std::pair<std::string, std::string>> remotes;

  git_strarray names{};
  if (git_remote_list(&names, repo_.get()) != 0) {
    return remotes;
  }

  for (std::size_t i = 0; i < names.count; ++i) {
    const char* name = names.strings[i];
    if (name == nullptr) {
      continue;
    }
    git_remote* remote = nullptr;
    if (git_remote_lookup(&remote, repo_.get(), name) != 0) {
      continue;
    }
    const char* url = git_remote_url(remote);
    // A remote with no fetch URL is push-only; there is nothing to query.
    if (url != nullptr) {
      remotes.emplace_back(name, url);
    }
    git_remote_free(remote);
  }

  git_strarray_dispose(&names);
  return remotes;
}

model::StatusSnapshot Repository::ReadStatus() const {
  model::StatusSnapshot snap;
  git_repository* repo = repo_.get();

  snap.repo_name = BaseName(WorkdirPath());

  git_reference* head = nullptr;
  const int head_rc = git_repository_head(&head, repo);
  if (head_rc == 0) {
    const char* name = git_reference_shorthand(head);
    snap.branch = name != nullptr ? name : "HEAD";
    snap.head_detached = git_repository_head_detached(repo) == 1;
    ReadTracking(repo, head, &snap);
    ReadRecent(repo, head, &snap);
    git_reference_free(head);
  } else if (head_rc == GIT_EUNBORNBRANCH) {
    snap.head_unborn = true;
    git_reference* sym = nullptr;
    if (git_reference_lookup(&sym, repo, "HEAD") == 0) {
      const char* target = git_reference_symbolic_target(sym);
      constexpr std::string_view kPrefix = "refs/heads/";
      const std::string t = target != nullptr ? target : "";
      snap.branch = t.rfind(kPrefix, 0) == 0 ? t.substr(kPrefix.size()) : t;
      git_reference_free(sym);
    }
    if (snap.branch.empty()) {
      snap.branch = "(unborn)";
    }
  } else {
    snap.branch = "(unknown)";
  }

  git_status_options opts;
  git_status_options_init(&opts, GIT_STATUS_OPTIONS_VERSION);
  opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
  opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS |
               GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX | GIT_STATUS_OPT_RENAMES_INDEX_TO_WORKDIR |
               GIT_STATUS_OPT_SORT_CASE_SENSITIVELY;

  git_status_list* list = nullptr;
  if (git_status_list_new(&list, repo, &opts) != 0) {
    return snap;
  }

  std::vector<StatusEntry> conflicts;
  std::vector<StatusEntry> staged;
  std::vector<StatusEntry> unstaged;
  std::vector<StatusEntry> untracked;

  const std::size_t count = git_status_list_entrycount(list);
  for (std::size_t i = 0; i < count; ++i) {
    const git_status_entry* e = git_status_byindex(list, i);
    if (e == nullptr) {
      continue;
    }
    const unsigned int s = e->status;

    if ((s & GIT_STATUS_CONFLICTED) != 0) {
      StatusEntry entry;
      entry.stage = Stage::Conflict;
      entry.change = Change::Modified;
      entry.path = PathOf(e);
      conflicts.push_back(std::move(entry));
      continue;
    }

    // One file can land in both buckets: staged rename plus a later edit, for
    // instance. Listing it twice is what lets each half be staged separately.
    Change index_change = Change::None;
    if ((s & GIT_STATUS_INDEX_NEW) != 0) {
      index_change = Change::Added;
    } else if ((s & GIT_STATUS_INDEX_MODIFIED) != 0) {
      index_change = Change::Modified;
    } else if ((s & GIT_STATUS_INDEX_DELETED) != 0) {
      index_change = Change::Deleted;
    } else if ((s & GIT_STATUS_INDEX_RENAMED) != 0) {
      index_change = Change::Renamed;
    } else if ((s & GIT_STATUS_INDEX_TYPECHANGE) != 0) {
      index_change = Change::TypeChange;
    }

    if (index_change != Change::None) {
      StatusEntry entry;
      entry.stage = Stage::Index;
      entry.change = index_change;
      const git_diff_delta* d = e->head_to_index;
      entry.path = (d != nullptr && d->new_file.path != nullptr) ? d->new_file.path : PathOf(e);
      if (index_change == Change::Renamed && d != nullptr && d->old_file.path != nullptr) {
        entry.old_path = d->old_file.path;
      }
      staged.push_back(std::move(entry));
    }

    Change wt_change = Change::None;
    if ((s & GIT_STATUS_WT_NEW) != 0) {
      wt_change = Change::Untracked;
    } else if ((s & GIT_STATUS_WT_MODIFIED) != 0) {
      wt_change = Change::Modified;
    } else if ((s & GIT_STATUS_WT_DELETED) != 0) {
      wt_change = Change::Deleted;
    } else if ((s & GIT_STATUS_WT_RENAMED) != 0) {
      wt_change = Change::Renamed;
    } else if ((s & GIT_STATUS_WT_TYPECHANGE) != 0) {
      wt_change = Change::TypeChange;
    }

    if (wt_change != Change::None) {
      StatusEntry entry;
      entry.stage = Stage::Worktree;
      entry.change = wt_change;
      const git_diff_delta* d = e->index_to_workdir;
      entry.path = (d != nullptr && d->new_file.path != nullptr) ? d->new_file.path : PathOf(e);
      if (wt_change == Change::Renamed && d != nullptr && d->old_file.path != nullptr) {
        entry.old_path = d->old_file.path;
      }
      if (wt_change == Change::Untracked) {
        untracked.push_back(std::move(entry));
      } else {
        unstaged.push_back(std::move(entry));
      }
    }
  }

  git_status_list_free(list);

  snap.conflicted = conflicts.size();
  snap.staged = staged.size();
  snap.unstaged = unstaged.size();
  snap.untracked = untracked.size();

  snap.entries.reserve(snap.total());
  for (auto* bucket : {&conflicts, &staged, &unstaged, &untracked}) {
    for (auto& entry : *bucket) {
      snap.entries.push_back(std::move(entry));
    }
  }

  return snap;
}

model::HistorySnapshot Repository::ReadHistory(std::size_t max_commits,
                                               int activity_days) const {
  model::HistorySnapshot snap;
  git_repository* repo = repo_.get();

  if (activity_days > 0) {
    snap.activity.assign(static_cast<std::size_t>(activity_days), 0);
  }

  // Which refs point where, so the log can badge a commit with its branch
  // names. Collected once up front rather than per commit.
  std::unordered_map<std::string, std::vector<std::string>> refs_at;
  git_reference_iterator* ref_iter = nullptr;
  if (git_reference_iterator_new(&ref_iter, repo) == 0) {
    git_reference* ref = nullptr;
    while (git_reference_next(&ref, ref_iter) == 0) {
      git_object* peeled = nullptr;
      if (git_reference_peel(&peeled, ref, GIT_OBJECT_COMMIT) == 0) {
        const char* shorthand = git_reference_shorthand(ref);
        if (shorthand != nullptr) {
          refs_at[OidToString(git_object_id(peeled))].emplace_back(shorthand);
        }
        git_object_free(peeled);
      }
      git_reference_free(ref);
    }
    git_reference_iterator_free(ref_iter);
  }

  git_oid head_oid;
  const std::string head_id =
      git_reference_name_to_id(&head_oid, repo, "HEAD") == 0 ? OidToString(&head_oid) : "";

  git_revwalk* walk = nullptr;
  if (git_revwalk_new(&walk, repo) != 0) {
    return snap;
  }
  git_revwalk_sorting(walk, GIT_SORT_TIME | GIT_SORT_TOPOLOGICAL);
  // Push every local branch so side branches get lanes of their own, plus HEAD
  // in case it is detached and therefore not on any branch.
  git_revwalk_push_glob(walk, "refs/heads/*");
  git_revwalk_push_head(walk);

  const auto now = static_cast<std::int64_t>(std::time(nullptr));
  const std::int64_t today = now / kSecondsPerDay;
  snap.activity_start_day = today - static_cast<std::int64_t>(activity_days) + 1;

  // Bounded so opening a large repository stays instant. The log itself is
  // capped lower; the extra walk exists only to fill the activity window.
  constexpr std::size_t kMaxWalk = 6000;
  std::size_t walked = 0;
  git_oid oid;

  std::unordered_map<std::int64_t, int> per_day;
  std::unordered_map<std::string, int> per_author;
  std::int64_t oldest_day = today;
  bool saw_any = false;

  while (walked < kMaxWalk && git_revwalk_next(&oid, walk) == 0) {
    ++walked;

    git_commit* commit = nullptr;
    if (git_commit_lookup(&commit, repo, &oid) != 0) {
      continue;
    }

    const auto when = static_cast<std::int64_t>(git_commit_time(commit));
    const std::int64_t day = when / kSecondsPerDay;
    if (day >= snap.activity_start_day && day <= today) {
      const auto bucket = static_cast<std::size_t>(day - snap.activity_start_day);
      if (bucket < snap.activity.size()) {
        snap.activity[bucket]++;
      }
    }

    const git_signature* author = git_commit_author(commit);

    // Stats cover the whole walk, not only the rows the log kept.
    if (day <= today) {
      per_day[day]++;
      oldest_day = saw_any ? std::min(oldest_day, day) : day;
      saw_any = true;

      snap.weekday[static_cast<std::size_t>((((day + 4) % 7) + 7) % 7)]++;

      // Shift into the author's own timezone so "commits at 2am" means their
      // 2am, not the reader's.
      const auto offset_minutes = static_cast<std::int64_t>(git_commit_time_offset(commit));
      const std::int64_t local = when + (offset_minutes * 60);
      const std::int64_t seconds_into_day = (((local % kSecondsPerDay) + kSecondsPerDay) %
                                             kSecondsPerDay);
      snap.hour[static_cast<std::size_t>(seconds_into_day / 3600)]++;
    }
    if (author != nullptr && author->name != nullptr) {
      per_author[author->name]++;
    }

    if (snap.commits.size() < max_commits) {
      model::Commit entry;
      entry.id = OidToString(&oid);
      entry.short_id = entry.id.substr(0, 7);
      entry.time = when;
      entry.is_head = !head_id.empty() && entry.id == head_id;

      const char* summary = git_commit_summary(commit);
      entry.summary = summary != nullptr ? summary : "(no message)";

      if (author != nullptr && author->name != nullptr) {
        entry.author = author->name;
      }

      const unsigned int parents = git_commit_parentcount(commit);
      entry.parents.reserve(parents);
      for (unsigned int p = 0; p < parents; ++p) {
        entry.parents.push_back(OidToString(git_commit_parent_id(commit, p)));
      }

      if (const auto found = refs_at.find(entry.id); found != refs_at.end()) {
        entry.refs = found->second;
      }

      snap.commits.push_back(std::move(entry));
    }

    git_commit_free(commit);
  }

  git_revwalk_free(walk);

  // Materialise the sparse day counts into a contiguous run so the chart can
  // index straight into it. Quiet days have to exist as zeroes or the timeline
  // would compress every gap out of the plot.
  if (saw_any && oldest_day <= today) {
    snap.daily_start_day = oldest_day;
    snap.daily.assign(static_cast<std::size_t>(today - oldest_day + 1), 0);
    for (const auto& [day, count] : per_day) {
      const auto index = static_cast<std::size_t>(day - oldest_day);
      if (index < snap.daily.size()) {
        snap.daily[index] = count;
      }
    }
  }

  snap.authors.assign(per_author.begin(), per_author.end());
  std::sort(snap.authors.begin(), snap.authors.end(),
            [](const auto& a, const auto& b) {
              if (a.second != b.second) {
                return a.second > b.second;
              }
              return a.first < b.first;
            });
  constexpr std::size_t kMaxAuthors = 12;
  if (snap.authors.size() > kMaxAuthors) {
    snap.authors.resize(kMaxAuthors);
  }

  snap.walked = walked;
  snap.truncated = walked >= kMaxWalk;
  if (!snap.activity.empty()) {
    snap.activity_max = *std::max_element(snap.activity.begin(), snap.activity.end());
  }
  AssignLanes(snap.commits);

  // ------------------------------------------------------------- branches
  git_branch_iterator* branch_iter = nullptr;
  if (git_branch_iterator_new(&branch_iter, repo, GIT_BRANCH_LOCAL) == 0) {
    git_reference* ref = nullptr;
    git_branch_t branch_type = GIT_BRANCH_LOCAL;

    while (git_branch_next(&ref, &branch_type, branch_iter) == 0) {
      model::Branch branch;
      const char* name = nullptr;
      if (git_branch_name(&name, ref) == 0 && name != nullptr) {
        branch.name = name;
      }
      branch.is_head = git_branch_is_head(ref) == 1;

      git_reference* upstream = nullptr;
      if (git_branch_upstream(&upstream, ref) == 0) {
        branch.has_upstream = true;
        const char* upstream_name = nullptr;
        if (git_branch_name(&upstream_name, upstream) == 0 && upstream_name != nullptr) {
          branch.upstream = upstream_name;
        }
        const git_oid* local_tip = git_reference_target(ref);
        const git_oid* remote_tip = git_reference_target(upstream);
        if (local_tip != nullptr && remote_tip != nullptr) {
          std::size_t ahead = 0;
          std::size_t behind = 0;
          if (git_graph_ahead_behind(&ahead, &behind, repo, local_tip, remote_tip) == 0) {
            branch.ahead = ahead;
            branch.behind = behind;
          }
        }
        git_reference_free(upstream);
      } else {
        branch.upstream = ConfiguredUpstream(repo, ref);
        branch.upstream_gone = !branch.upstream.empty();
      }

      if (const git_oid* tip = git_reference_target(ref); tip != nullptr) {
        git_commit* tip_commit = nullptr;
        if (git_commit_lookup(&tip_commit, repo, tip) == 0) {
          branch.time = static_cast<std::int64_t>(git_commit_time(tip_commit));
          git_commit_free(tip_commit);
        }
        ReadVelocity(repo, tip, today, &branch);
      }

      snap.branches.push_back(std::move(branch));
      git_reference_free(ref);
    }
    git_branch_iterator_free(branch_iter);
  }

  // Most recently touched first, with the checked-out branch pinned to the top.
  std::sort(snap.branches.begin(), snap.branches.end(),
            [](const model::Branch& a, const model::Branch& b) {
              if (a.is_head != b.is_head) {
                return a.is_head;
              }
              return a.time > b.time;
            });

  return snap;
}

OpResult Repository::Stage(const StatusEntry& entry) {
  IndexHandle h;
  if (git_repository_index(&h.index, repo_.get()) != 0) {
    return OpResult::Fail(LastError());
  }

  // A file deleted in the working tree has to be removed from the index; adding
  // it by path would fail because there is nothing on disk to read.
  const bool deleted_on_disk = entry.change == Change::Deleted && entry.stage != Stage::Index;
  const int rc = deleted_on_disk ? git_index_remove_bypath(h.index, entry.path.c_str())
                                 : git_index_add_bypath(h.index, entry.path.c_str());
  if (rc != 0) {
    return OpResult::Fail(LastError());
  }
  if (git_index_write(h.index) != 0) {
    return OpResult::Fail(LastError());
  }

  if (entry.stage == Stage::Conflict) {
    return OpResult::Ok("resolved and staged " + entry.path);
  }
  return OpResult::Ok("staged " + entry.path);
}

OpResult Repository::Unstage(const StatusEntry& entry) {
  git_object* head_commit = nullptr;
  if (git_revparse_single(&head_commit, repo_.get(), "HEAD") == 0) {
    char* raw_path = const_cast<char*>(entry.path.c_str());
    git_strarray paths{&raw_path, 1};
    const int rc = git_reset_default(repo_.get(), head_commit, &paths);
    git_object_free(head_commit);
    if (rc != 0) {
      return OpResult::Fail(LastError());
    }
    return OpResult::Ok("unstaged " + entry.path);
  }

  // Unborn HEAD: no commit to reset against, so drop the path from the index.
  IndexHandle h;
  if (git_repository_index(&h.index, repo_.get()) != 0) {
    return OpResult::Fail(LastError());
  }
  if (git_index_remove_bypath(h.index, entry.path.c_str()) != 0) {
    return OpResult::Fail(LastError());
  }
  if (git_index_write(h.index) != 0) {
    return OpResult::Fail(LastError());
  }
  return OpResult::Ok("unstaged " + entry.path);
}

OpResult Repository::StageAll() {
  IndexHandle h;
  if (git_repository_index(&h.index, repo_.get()) != 0) {
    return OpResult::Fail(LastError());
  }

  char* pattern = const_cast<char*>("*");
  git_strarray paths{&pattern, 1};

  if (git_index_add_all(h.index, &paths, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr) != 0) {
    return OpResult::Fail(LastError());
  }
  // add_all skips files deleted from disk; update_all is what records those.
  if (git_index_update_all(h.index, &paths, nullptr, nullptr) != 0) {
    return OpResult::Fail(LastError());
  }
  if (git_index_write(h.index) != 0) {
    return OpResult::Fail(LastError());
  }
  return OpResult::Ok("staged everything");
}

OpResult Repository::Discard(const StatusEntry& entry) {
  if (entry.stage == Stage::Index) {
    return OpResult::Fail("unstage this first, then discard");
  }

  if (entry.change == Change::Untracked) {
    const std::filesystem::path target = std::filesystem::path(WorkdirPath()) / entry.path;
    std::error_code ec;
    std::filesystem::remove(target, ec);
    if (ec) {
      return OpResult::Fail("could not delete " + entry.path + ": " + ec.message());
    }
    return OpResult::Ok("deleted " + entry.path);
  }

  // Restores from the index rather than from HEAD, matching `git restore`. A
  // file that is both staged and edited keeps its staged content.
  git_checkout_options co;
  git_checkout_options_init(&co, GIT_CHECKOUT_OPTIONS_VERSION);
  co.checkout_strategy = GIT_CHECKOUT_FORCE | GIT_CHECKOUT_DISABLE_PATHSPEC_MATCH;

  char* raw_path = const_cast<char*>(entry.path.c_str());
  co.paths.strings = &raw_path;
  co.paths.count = 1;

  if (git_checkout_index(repo_.get(), nullptr, &co) != 0) {
    return OpResult::Fail(LastError());
  }
  return OpResult::Ok("discarded changes in " + entry.path);
}

OpResult Repository::Commit(const std::string& message) {
  if (message.empty()) {
    return OpResult::Fail("commit message is empty");
  }

  git_repository* repo = repo_.get();

  // A rebase writes its commits through git_rebase_commit, which advances the
  // plan as well as the branch. An ordinary commit here would look like it
  // worked and leave the rebase standing on a step it had already applied.
  if (git_repository_state(repo) == GIT_REPOSITORY_STATE_REBASE ||
      git_repository_state(repo) == GIT_REPOSITORY_STATE_REBASE_INTERACTIVE ||
      git_repository_state(repo) == GIT_REPOSITORY_STATE_REBASE_MERGE) {
    return OpResult::Fail("a rebase is in progress — continue it instead of committing");
  }

  IndexHandle h;
  if (git_repository_index(&h.index, repo) != 0) {
    return OpResult::Fail(LastError());
  }
  if (git_index_has_conflicts(h.index) == 1) {
    return OpResult::Fail("resolve conflicts before committing");
  }

  git_oid tree_oid;
  if (git_index_write_tree(&tree_oid, h.index) != 0) {
    return OpResult::Fail(LastError());
  }

  git_tree* tree = nullptr;
  if (git_tree_lookup(&tree, repo, &tree_oid) != 0) {
    return OpResult::Fail(LastError());
  }

  git_signature* sig = nullptr;
  if (git_signature_default(&sig, repo) != 0) {
    git_tree_free(tree);
    return OpResult::Fail("set user.name and user.email before committing");
  }

  // HEAD first, then whatever MERGE_HEAD names. Committing a merge with one
  // parent produces a commit that claims the other side never happened and
  // leaves MERGE_HEAD on disk for the next command to trip over — and it looks
  // like it worked, which is what makes it worth the extra dozen lines.
  ParentList parents;
  git_oid head_oid;
  git_commit* head = nullptr;
  const bool has_head = git_reference_name_to_id(&head_oid, repo, "HEAD") == 0 &&
                        git_commit_lookup(&head, repo, &head_oid) == 0;
  if (has_head) {
    parents.commits.push_back(head);
  }
  MergeHeadPayload payload{repo, &parents};
  git_repository_mergehead_foreach(repo, MergeHeadCb, &payload);
  const bool merging = parents.commits.size() > 1;

  git_oid commit_oid;
  const int rc = git_commit_create(&commit_oid, repo, "HEAD", sig, sig, nullptr, message.c_str(),
                                   tree, parents.commits.size(),
                                   parents.commits.empty()
                                       ? nullptr
                                       : const_cast<const git_commit**>(parents.commits.data()));

  git_signature_free(sig);
  git_tree_free(tree);

  if (rc != 0) {
    return OpResult::Fail(LastError());
  }

  // MERGE_HEAD and MERGE_MSG only go away when something clears them, and until
  // they do the repository still reports itself as mid-merge.
  if (merging) {
    git_repository_state_cleanup(repo);
  }

  char short_id[8] = {};
  git_oid_tostr(short_id, sizeof(short_id), &commit_oid);
  return OpResult::Ok(std::string(merging ? "merge committed " : "committed ") + short_id);
}

}  // namespace gittop::git
