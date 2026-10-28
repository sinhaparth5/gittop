#include <git2.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "git/internal.hpp"
#include "git/repository.hpp"

// Repository::ReadDiff lives here rather than in repository.cpp because the
// four libgit2 callbacks it needs are a file's worth of code on their own and
// have nothing to say to staging or to the revwalk.

namespace gittop::git {
namespace {

using model::DiffFile;
using model::DiffLine;
using model::DiffLineKind;
using model::DiffSnapshot;
using model::DiffSource;

model::Change FromDelta(git_delta_t status) {
  switch (status) {
    case GIT_DELTA_ADDED:
    case GIT_DELTA_COPIED:
      return model::Change::Added;
    case GIT_DELTA_DELETED:
      return model::Change::Deleted;
    case GIT_DELTA_RENAMED:
      return model::Change::Renamed;
    case GIT_DELTA_TYPECHANGE:
      return model::Change::TypeChange;
    case GIT_DELTA_UNTRACKED:
      return model::Change::Untracked;
    default:
      break;
  }
  return model::Change::Modified;
}

// libgit2 hands back the line with whatever terminator the file had, and a
// terminal that is given a \r draws the rest of the row on top of the gutter.
std::string Chomp(const char* content, std::size_t length) {
  while (length > 0 && (content[length - 1] == '\n' || content[length - 1] == '\r')) {
    --length;
  }
  return std::string(content, length);
}

// Everything the callbacks share. `stopped` is how a cap is enforced: libgit2
// only stops iterating when a callback returns non-zero, and that return is
// indistinguishable from a real failure by the time git_diff_foreach reports
// it — so the reason is recorded here instead of read off the return code.
struct Sink {
  DiffSnapshot* out = nullptr;
  std::size_t max_lines = 0;
  bool stopped = false;

  bool Full() const { return out->lines.size() >= max_lines; }

  void Push(DiffLineKind kind, std::string text, int old_lineno = -1, int new_lineno = -1) {
    out->lines.push_back(DiffLine{kind, std::move(text), old_lineno, new_lineno});
  }
};

int FileCb(const git_diff_delta* delta, float /*progress*/, void* payload) {
  auto* sink = static_cast<Sink*>(payload);
  if (sink->Full()) {
    sink->stopped = true;
    return 1;
  }

  DiffFile file;
  file.change = FromDelta(delta->status);
  file.path = delta->new_file.path != nullptr ? delta->new_file.path : "";
  if (file.path.empty() && delta->old_file.path != nullptr) {
    file.path = delta->old_file.path;
  }
  if (file.change == model::Change::Renamed && delta->old_file.path != nullptr) {
    file.old_path = delta->old_file.path;
  }
  file.binary = (delta->flags & GIT_DIFF_FLAG_BINARY) != 0;
  file.first_line = sink->out->lines.size();

  sink->out->files.push_back(std::move(file));
  sink->Push(DiffLineKind::FileHeader, sink->out->files.back().path);

  // A binary delta produces no hunks at all, so without this the file banner
  // would be followed by the next file's and read as an empty change.
  if (sink->out->files.back().binary) {
    sink->Push(DiffLineKind::Binary, "binary file — not shown");
  }
  return 0;
}

int HunkCb(const git_diff_delta* /*delta*/, const git_diff_hunk* hunk, void* payload) {
  auto* sink = static_cast<Sink*>(payload);
  if (sink->Full()) {
    sink->stopped = true;
    return 1;
  }
  // hunk->header is NUL-terminated but carries its own newline, and may also
  // carry the enclosing function's signature, which is worth keeping.
  sink->Push(DiffLineKind::HunkHeader, Chomp(hunk->header, hunk->header_len));
  return 0;
}

int LineCb(const git_diff_delta* /*delta*/, const git_diff_hunk* /*hunk*/,
           const git_diff_line* line, void* payload) {
  auto* sink = static_cast<Sink*>(payload);
  if (sink->Full()) {
    sink->stopped = true;
    return 1;
  }

  DiffLineKind kind = DiffLineKind::Context;
  switch (line->origin) {
    case GIT_DIFF_LINE_ADDITION:
      kind = DiffLineKind::Added;
      break;
    case GIT_DIFF_LINE_DELETION:
      kind = DiffLineKind::Removed;
      break;
    case GIT_DIFF_LINE_ADD_EOFNL:
    case GIT_DIFF_LINE_DEL_EOFNL:
    case GIT_DIFF_LINE_CONTEXT_EOFNL:
      // "\ No newline at end of file". It is a note about the file rather than
      // a line of it, and colouring it as content says something untrue.
      sink->Push(DiffLineKind::Note, Chomp(line->content, line->content_len));
      return 0;
    default:
      break;
  }

  if (!sink->out->files.empty()) {
    DiffFile& file = sink->out->files.back();
    if (kind == DiffLineKind::Added) {
      ++file.additions;
      ++sink->out->additions;
    } else if (kind == DiffLineKind::Removed) {
      ++file.deletions;
      ++sink->out->deletions;
    }
  }

  sink->Push(kind, Chomp(line->content, line->content_len),
             line->old_lineno > 0 ? line->old_lineno : -1,
             line->new_lineno > 0 ? line->new_lineno : -1);
  return 0;
}

// Frees whichever of these an early return leaves behind. Six goto-free exits
// through four libgit2 handles is otherwise a leak waiting for one of them.
struct DiffHandles {
  git_diff* diff = nullptr;
  git_tree* left = nullptr;
  git_tree* right = nullptr;
  git_commit* commit = nullptr;
  git_object* object = nullptr;
  git_patch* patch = nullptr;

  ~DiffHandles() {
    if (patch != nullptr) git_patch_free(patch);
    if (diff != nullptr) git_diff_free(diff);
    if (left != nullptr) git_tree_free(left);
    if (right != nullptr) git_tree_free(right);
    if (commit != nullptr) git_commit_free(commit);
    if (object != nullptr) git_object_free(object);
  }
};

// The two diffs that touch the index. An unborn HEAD has no tree, and libgit2
// reads a null tree as "empty", which is exactly right: everything in the index
// is an addition.
int DiffIndex(DiffHandles& handles, git_repository* repo, DiffSource source,
              const git_diff_options& opts) {
  if (source == DiffSource::Worktree) {
    return git_diff_index_to_workdir(&handles.diff, repo, nullptr, &opts);
  }
  git_reference* head = nullptr;
  if (git_repository_head(&head, repo) == 0) {
    git_commit* head_commit = nullptr;
    if (git_reference_peel(reinterpret_cast<git_object**>(&head_commit), head,
                           GIT_OBJECT_COMMIT) == 0) {
      git_commit_tree(&handles.left, head_commit);
      git_commit_free(head_commit);
    }
    git_reference_free(head);
  }
  return git_diff_tree_to_index(&handles.diff, repo, handles.left, nullptr, &opts);
}

}  // namespace

DiffSnapshot Repository::ReadDiff(const DiffRequest& request, std::size_t max_lines) const {
  DiffSnapshot snap;
  snap.source = request.source;
  snap.subtitle = request.path;

  git_repository* repo = repo_.get();
  DiffHandles handles;

  // The function form rather than GIT_DIFF_OPTIONS_INIT: the macro is a partial
  // initializer and every field it leaves out is a -Wmissing-field-initializers
  // warning in a build that asks for those.
  git_diff_options opts;
  git_diff_options_init(&opts, GIT_DIFF_OPTIONS_VERSION);
  opts.context_lines = 3;
  // A new file with no content shown is a diff that says a file appeared and
  // refuses to say what is in it, which is the one question being asked.
  opts.flags = GIT_DIFF_INCLUDE_UNTRACKED | GIT_DIFF_SHOW_UNTRACKED_CONTENT |
               GIT_DIFF_RECURSE_UNTRACKED_DIRS;

  // pathspec borrows the caller's string, so `path` has to outlive the call —
  // it does, since it lives in `request`.
  const char* pathspec = request.path.c_str();
  char* pathspec_array[1] = {const_cast<char*>(pathspec)};
  if (!request.path.empty()) {
    opts.pathspec.strings = pathspec_array;
    opts.pathspec.count = 1;
  }

  switch (request.source) {
    case DiffSource::Worktree:
    case DiffSource::Staged: {
      snap.title = request.source == DiffSource::Worktree ? "working tree against the index"
                                                          : "index against HEAD";
      if (DiffIndex(handles, repo, request.source, opts) != 0) {
        snap.error = LastError();
        return snap;
      }
      break;
    }

    case DiffSource::Commit: {
      if (request.commit.empty()) {
        snap.error = "no commit to show";
        return snap;
      }
      if (git_revparse_single(&handles.object, repo, request.commit.c_str()) != 0) {
        snap.error = LastError();
        return snap;
      }
      if (git_object_peel(reinterpret_cast<git_object**>(&handles.commit), handles.object,
                          GIT_OBJECT_COMMIT) != 0) {
        snap.error = LastError();
        return snap;
      }

      const char* summary = git_commit_summary(handles.commit);
      snap.title = summary != nullptr ? summary : "commit";
      snap.subtitle = request.commit.substr(0, 12);

      if (git_commit_tree(&handles.right, handles.commit) != 0) {
        snap.error = LastError();
        return snap;
      }
      // A root commit has no parent, and a null left tree makes every file in
      // it an addition — which is what `git show` prints for one too.
      if (git_commit_parentcount(handles.commit) > 0) {
        git_commit* parent = nullptr;
        if (git_commit_parent(&parent, handles.commit, 0) == 0) {
          git_commit_tree(&handles.left, parent);
          git_commit_free(parent);
        }
      }
      if (git_diff_tree_to_tree(&handles.diff, repo, handles.left, handles.right, &opts) != 0) {
        snap.error = LastError();
        return snap;
      }
      break;
    }
  }

  // Rename detection is off by default and is the difference between one row
  // saying a file moved and two rows claiming a deletion and an unrelated new
  // file. Failing to detect is not worth abandoning the diff over.
  git_diff_find_similar(handles.diff, nullptr);

  Sink sink{&snap, max_lines, false};
  const int rc = git_diff_foreach(handles.diff, FileCb, /*binary_cb=*/nullptr, HunkCb, LineCb,
                                  &sink);
  if (rc != 0 && !sink.stopped) {
    snap.error = LastError();
    return snap;
  }
  snap.truncated = sink.stopped;
  if (snap.truncated) {
    snap.lines.push_back(DiffLine{DiffLineKind::Note,
                                  "diff truncated — " + std::to_string(max_lines) +
                                      " line cap reached",
                                  -1, -1});
  }
  return snap;
}

OpResult Repository::ApplyHunk(const HunkPick& pick) {
  if (pick.source == DiffSource::Commit) {
    return OpResult::Fail("a commit is history — there is nothing in it to stage");
  }
  git_repository* repo = repo_.get();
  DiffHandles handles;

  // The same three lines of context ReadDiff asks for, or hunk n here would be
  // a different hunk from the n-th one on screen.
  git_diff_options opts;
  git_diff_options_init(&opts, GIT_DIFF_OPTIONS_VERSION);
  opts.context_lines = 3;
  opts.flags = GIT_DIFF_DISABLE_PATHSPEC_MATCH;
  char* pathspec[1] = {const_cast<char*>(pick.path.c_str())};
  opts.pathspec.strings = pathspec;
  opts.pathspec.count = 1;

  if (DiffIndex(handles, repo, pick.source, opts) != 0) {
    return OpResult::Fail(LastError());
  }
  if (git_diff_num_deltas(handles.diff) != 1) {
    return OpResult::Fail("the diff changed — press r");
  }
  if (git_diff_get_delta(handles.diff, 0)->status != GIT_DELTA_MODIFIED) {
    return OpResult::Fail("a new, deleted or renamed file stages whole, from the status view");
  }
  if (git_patch_from_diff(&handles.patch, handles.diff, 0) != 0) {
    return OpResult::Fail(LastError());
  }

  const git_diff_hunk* hunk = nullptr;
  std::size_t count = 0;
  if (pick.hunk >= git_patch_num_hunks(handles.patch) ||
      git_patch_get_hunk(&hunk, &count, handles.patch, pick.hunk) != 0 ||
      Chomp(hunk->header, hunk->header_len) != pick.header) {
    return OpResult::Fail("the diff changed — press r");
  }

  // Unstaging is staging the reverse of the staged diff: flip every + and -
  // and read the hunk from its new side, and the rule below serves both.
  const bool reverse = pick.source == DiffSource::Staged;
  std::string body;
  int old_count = 0;
  int new_count = 0;
  int content = -1;
  bool changes = false;
  for (std::size_t i = 0; i < count; ++i) {
    const git_diff_line* line = nullptr;
    if (git_patch_get_line_in_hunk(&line, handles.patch, pick.hunk, i) != 0) {
      return OpResult::Fail(LastError());
    }
    char origin = line->origin;
    // The "no newline at end of file" markers are rebuilt from the content
    // below, so they are not counted as lines the cursor can stand on either —
    // ReadDiff turns them into notes.
    if (origin != GIT_DIFF_LINE_ADDITION && origin != GIT_DIFF_LINE_DELETION &&
        origin != GIT_DIFF_LINE_CONTEXT) {
      continue;
    }
    ++content;
    if (reverse && origin != ' ') {
      origin = origin == '+' ? '-' : '+';
    }
    const bool picked = pick.line < 0 || pick.line == content;
    // An addition left out never happens; a removal left out keeps its line.
    if (!picked && origin == '+') {
      continue;
    }
    if (!picked && origin == '-') {
      origin = ' ';
    }
    changes = changes || origin != ' ';
    old_count += origin != '+' ? 1 : 0;
    new_count += origin != '-' ? 1 : 0;
    body += origin;
    body.append(line->content, line->content_len);
    if (body.back() != '\n') {
      body += "\n\\ No newline at end of file\n";
    }
  }
  if (!changes) {
    return OpResult::Fail("a context line has nothing to stage");
  }

  // libgit2 places a hunk by its new start, which for a pure insertion is one
  // past the line it goes after.
  const int start = reverse ? hunk->new_start : hunk->old_start;
  const int new_start = old_count == 0 ? start + 1 : start;
  const std::string patch = "diff --git a/" + pick.path + " b/" + pick.path + "\n--- a/" +
                            pick.path + "\n+++ b/" + pick.path + "\n@@ -" +
                            std::to_string(start) + "," + std::to_string(old_count) + " +" +
                            std::to_string(new_start) + "," + std::to_string(new_count) +
                            " @@\n" + body;

  git_diff* parsed = nullptr;
  if (git_diff_from_buffer(&parsed, patch.data(), patch.size()) != 0) {
    return OpResult::Fail(LastError());
  }
  const int rc = git_apply(repo, parsed, GIT_APPLY_LOCATION_INDEX, nullptr);
  git_diff_free(parsed);
  if (rc != 0) {
    return OpResult::Fail(LastError());
  }

  const char* verb = reverse ? "unstaged" : "staged";
  return OpResult::Ok(std::string(verb) + (pick.line < 0 ? " hunk in " : " line in ") +
                      pick.path);
}

}  // namespace gittop::git
