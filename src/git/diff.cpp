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

  ~DiffHandles() {
    if (diff != nullptr) git_diff_free(diff);
    if (left != nullptr) git_tree_free(left);
    if (right != nullptr) git_tree_free(right);
    if (commit != nullptr) git_commit_free(commit);
    if (object != nullptr) git_object_free(object);
  }
};

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
    case DiffSource::Worktree: {
      snap.title = "working tree against the index";
      if (git_diff_index_to_workdir(&handles.diff, repo, nullptr, &opts) != 0) {
        snap.error = LastError();
        return snap;
      }
      break;
    }

    case DiffSource::Staged: {
      snap.title = "index against HEAD";
      // An unborn HEAD has no tree, and libgit2 reads a null tree as "empty",
      // which is exactly right: everything in the index is an addition.
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
      if (git_diff_tree_to_index(&handles.diff, repo, handles.left, nullptr, &opts) != 0) {
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

}  // namespace gittop::git
