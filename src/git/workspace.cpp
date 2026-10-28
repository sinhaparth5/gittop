#include <git2.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "git/internal.hpp"
#include "git/repository.hpp"

// Worktrees and submodules: the other checkouts that hang off one repository.
// Everything here is local. Updating a submodule clones or fetches, so that
// one lives in git/transfer.cpp and runs on a worker like every other transfer.

namespace gittop::git {
namespace {

std::string Short(const git_oid* id) {
  char out[8] = {};
  git_oid_tostr(out, sizeof(out), id);
  return out;
}

// libgit2 ends a workdir with a separator and a worktree path without one, so
// both are compared and printed without.
std::string Trim(std::string path) {
  while (path.size() > 1 && (path.back() == '/' || path.back() == '\\')) {
    path.pop_back();
  }
  return path;
}

int CountChanges(git_repository* repo) {
  git_status_options opts;
  git_status_options_init(&opts, GIT_STATUS_OPTIONS_VERSION);
  opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED;
  git_status_list* list = nullptr;
  if (git_status_list_new(&list, repo, &opts) != 0) {
    return 0;
  }
  const int n = static_cast<int>(git_status_list_entrycount(list));
  git_status_list_free(list);
  return n;
}

// Path, HEAD and changes of one open working tree.
model::Worktree Describe(git_repository* repo) {
  model::Worktree tree;
  const char* workdir = git_repository_workdir(repo);
  tree.path = Trim(workdir != nullptr ? workdir : "");
  git_reference* head = nullptr;
  if (git_repository_head(&head, repo) == 0) {
    tree.detached = git_repository_head_detached(repo) == 1;
    tree.branch = tree.detached ? Short(git_reference_target(head)) : git_reference_shorthand(head);
    git_reference_free(head);
  }
  tree.changes = CountChanges(repo);
  return tree;
}

// The main working tree, which is where the repository's own .git lives. Read
// through the common dir rather than assumed to be repo_, because gittop may
// have been opened inside a linked worktree.
bool OpenMain(git_repository* repo, git_repository** out) {
  if (git_repository_open(out, git_repository_commondir(repo)) != 0) {
    return false;
  }
  if (git_repository_is_bare(*out) == 1) {
    git_repository_free(*out);
    *out = nullptr;
    return false;
  }
  return true;
}

}  // namespace

model::Workspace Repository::ReadWorkspace() const {
  git_repository* repo = repo_.get();
  model::Workspace out;
  const std::string here = Trim(WorkdirPath());

  git_repository* main = nullptr;
  if (OpenMain(repo, &main)) {
    model::Worktree tree = Describe(main);
    tree.main = true;
    tree.current = tree.path == here;
    out.worktrees.push_back(std::move(tree));
    git_repository_free(main);
  }

  git_strarray names{};
  if (git_worktree_list(&names, repo) == 0) {
    for (std::size_t i = 0; i < names.count; ++i) {
      git_worktree* wt = nullptr;
      if (git_worktree_lookup(&wt, repo, names.strings[i]) != 0) {
        continue;
      }
      model::Worktree tree;
      tree.locked = git_worktree_is_locked(nullptr, wt) > 0;
      tree.missing = git_worktree_validate(wt) != 0;
      git_repository* linked = nullptr;
      if (!tree.missing && git_repository_open_from_worktree(&linked, wt) == 0) {
        tree = Describe(linked);
        tree.locked = git_worktree_is_locked(nullptr, wt) > 0;
        git_repository_free(linked);
      }
      tree.name = names.strings[i];
      tree.path = Trim(git_worktree_path(wt));
      tree.current = tree.path == here;
      out.worktrees.push_back(std::move(tree));
      git_worktree_free(wt);
    }
    git_strarray_dispose(&names);
  }

  // Names first and lookups after: libgit2 does not want the submodule cache
  // touched from inside its own foreach.
  std::vector<std::string> modules;
  git_submodule_foreach(
      repo,
      [](git_submodule*, const char* name, void* payload) {
        static_cast<std::vector<std::string>*>(payload)->emplace_back(name);
        return 0;
      },
      &modules);
  for (const std::string& name : modules) {
    git_submodule* sm = nullptr;
    if (git_submodule_lookup(&sm, repo, name.c_str()) != 0) {
      continue;
    }
    model::Submodule& module = out.submodules.emplace_back();
    module.name = name;
    module.path = git_submodule_path(sm);
    const char* url = git_submodule_url(sm);
    module.url = url != nullptr ? url : "";
    if (const git_oid* id = git_submodule_head_id(sm); id != nullptr) {
      module.recorded = Short(id);
    }
    if (const git_oid* id = git_submodule_wd_id(sm); id != nullptr) {
      module.checked_out = Short(id);
      module.cloned = true;
    }
    unsigned int status = 0;
    if (git_submodule_status(&status, repo, name.c_str(), GIT_SUBMODULE_IGNORE_UNSPECIFIED) == 0) {
      module.dirty = (status & (GIT_SUBMODULE_STATUS_WD_INDEX_MODIFIED |
                                GIT_SUBMODULE_STATUS_WD_WD_MODIFIED |
                                GIT_SUBMODULE_STATUS_WD_UNTRACKED)) != 0;
    }
    git_submodule_free(sm);
  }
  std::sort(out.submodules.begin(), out.submodules.end(),
            [](const model::Submodule& a, const model::Submodule& b) { return a.path < b.path; });
  return out;
}

OpResult Repository::AddWorktree(const std::string& branch) {
  git_repository* repo = repo_.get();
  if (git_reference_is_valid_name(("refs/heads/" + branch).c_str()) != 1) {
    return OpResult::Fail("'" + branch + "' is not a valid branch name");
  }

  // Beside the main tree and named after it, the way people lay these out by
  // hand: ~/src/gittop and ~/src/gittop-feature-x.
  git_repository* main = nullptr;
  if (!OpenMain(repo, &main)) {
    return OpResult::Fail("could not find the main working tree");
  }
  const std::filesystem::path main_path = Trim(git_repository_workdir(main));
  git_repository_free(main);
  std::string slug = branch;
  std::replace(slug.begin(), slug.end(), '/', '-');
  const std::filesystem::path path =
      main_path.parent_path() / (main_path.filename().string() + "-" + slug);
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    return OpResult::Fail(path.string() + " already exists");
  }

  // A branch that does not exist yet starts at HEAD, which is `git worktree add
  // -b`. One that does is checked out as it is, and libgit2 refuses it if
  // another worktree already has it, as git does.
  git_reference* ref = nullptr;
  bool created = false;
  if (git_branch_lookup(&ref, repo, branch.c_str(), GIT_BRANCH_LOCAL) != 0) {
    git_object* head = nullptr;
    const bool ok = git_revparse_single(&head, repo, "HEAD^{commit}") == 0 &&
                    git_branch_create(&ref, repo, branch.c_str(),
                                      reinterpret_cast<git_commit*>(head), /*force=*/0) == 0;
    git_object_free(head);
    if (!ok) {
      return OpResult::Fail(LastError());
    }
    created = true;
  }

  git_worktree_add_options opts;
  git_worktree_add_options_init(&opts, GIT_WORKTREE_ADD_OPTIONS_VERSION);
  opts.ref = ref;
  git_worktree* wt = nullptr;
  OpResult result = OpResult::Ok("worktree for " + branch + (created ? " (new)" : "") + " at " +
                                 path.string());
  if (git_worktree_add(&wt, repo, slug.c_str(), path.string().c_str(), &opts) != 0) {
    result = OpResult::Fail(LastError());
    // A refused add leaves nothing behind, including the branch made for it.
    if (created) {
      git_branch_delete(ref);
    }
  }
  git_worktree_free(wt);
  git_reference_free(ref);
  return result;
}

OpResult Repository::RemoveWorktree(const std::string& name) {
  git_worktree* wt = nullptr;
  if (git_worktree_lookup(&wt, repo_.get(), name.c_str()) != 0) {
    return OpResult::Fail(LastError());
  }
  OpResult result = OpResult::Ok("removed worktree " + name);
  if (git_worktree_is_locked(nullptr, wt) > 0) {
    result = OpResult::Fail(name + " is locked — git worktree unlock it first");
  } else if (git_worktree_validate(wt) == 0) {
    // `git worktree remove`'s rule: the directory goes, so whatever is not
    // committed in it would go too.
    git_repository* linked = nullptr;
    if (git_repository_open_from_worktree(&linked, wt) == 0) {
      if (CountChanges(linked) > 0) {
        result = OpResult::Fail(name + " has uncommitted changes");
      }
      git_repository_free(linked);
    }
  } else {
    result = OpResult::Ok("pruned worktree " + name);
  }
  if (result.ok) {
    git_worktree_prune_options opts;
    git_worktree_prune_options_init(&opts, GIT_WORKTREE_PRUNE_OPTIONS_VERSION);
    opts.flags = GIT_WORKTREE_PRUNE_VALID | GIT_WORKTREE_PRUNE_WORKING_TREE;
    if (git_worktree_prune(wt, &opts) != 0) {
      result = OpResult::Fail(LastError());
    }
  }
  git_worktree_free(wt);
  return result;
}

}  // namespace gittop::git
