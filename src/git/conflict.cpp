#include <git2.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "git/internal.hpp"
#include "git/repository.hpp"

// Merging a branch, and resolving what a merge, cherry-pick, revert or rebase
// stopped on. The resolver works on the working-tree file's markers because
// that file is what the user sees and may already have edited; the index's
// three stages are only consulted for the whole-file answer, which is the one
// a deleted side or a binary file leaves.

namespace gittop::git {
namespace {

// A marker line: seven of `c`, then the end of the line or a space and a label.
// Returns false for anything else, including eight of them, which is content.
bool Marker(const std::string& line, char c, std::string* label) {
  if (line.size() < 7 || line.compare(0, 7, std::string(7, c)) != 0) {
    return false;
  }
  std::string rest = line.substr(7);
  if (!rest.empty() && rest.back() == '\r') {
    rest.pop_back();
  }
  if (!rest.empty() && rest.front() != ' ') {
    return false;
  }
  if (label != nullptr) {
    *label = rest.empty() ? std::string() : rest.substr(1);
  }
  return true;
}

std::string ReadFile(const std::filesystem::path& path, bool* ok) {
  std::ifstream in(path, std::ios::binary);
  *ok = static_cast<bool>(in);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool WriteFile(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  return static_cast<bool>(out);
}

// Stages `path` as the working tree has it, which also clears its conflict
// entries from the index — the `git add` that marks a conflict resolved.
OpResult StageResolved(git_repository* repo, const std::string& path, bool removed) {
  git_index* index = nullptr;
  if (git_repository_index(&index, repo) != 0) {
    return OpResult::Fail(LastError());
  }
  const int rc = removed ? git_index_remove_bypath(index, path.c_str())
                         : git_index_add_bypath(index, path.c_str());
  OpResult result = rc == 0 && git_index_write(index) == 0 ? OpResult::Ok() : OpResult::Fail(LastError());
  git_index_free(index);
  return result;
}

std::size_t CountConflicts(git_repository* repo) {
  git_index* index = nullptr;
  git_index_conflict_iterator* it = nullptr;
  std::size_t count = 0;
  if (git_repository_index(&index, repo) == 0 && git_index_conflict_iterator_new(&it, index) == 0) {
    const git_index_entry* ancestor = nullptr;
    const git_index_entry* ours = nullptr;
    const git_index_entry* theirs = nullptr;
    while (git_index_conflict_next(&ancestor, &ours, &theirs, it) == 0) {
      ++count;
    }
  }
  git_index_conflict_iterator_free(it);
  git_index_free(index);
  return count;
}

std::string Plural(std::size_t n, const char* word) {
  return std::to_string(n) + " " + word + (n == 1 ? "" : "s");
}

}  // namespace

void ParseConflictMarkers(const std::string& text, model::ConflictFile* out) {
  out->lines.clear();
  out->hunks.clear();
  out->unclosed = false;
  out->trailing_newline = !text.empty() && text.back() == '\n';
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t newline = text.find('\n', start);
    const std::size_t stop = newline == std::string::npos ? text.size() : newline;
    out->lines.push_back(text.substr(start, stop - start));
    start = stop + 1;
  }

  enum { kOutside, kOurs, kBase, kTheirs } where = kOutside;
  model::ConflictHunk hunk;
  for (std::size_t i = 0; i < out->lines.size(); ++i) {
    const std::string& line = out->lines[i];
    std::string label;
    if (where == kOutside) {
      if (Marker(line, '<', &label)) {
        hunk = model::ConflictHunk{};
        hunk.begin = i;
        hunk.ours_label = label;
        where = kOurs;
      }
    } else if (where == kOurs && Marker(line, '|', nullptr)) {
      where = kBase;
    } else if (where != kTheirs && Marker(line, '=', &label) && label.empty()) {
      where = kTheirs;
    } else if (where == kTheirs && Marker(line, '>', &label)) {
      hunk.end = i;
      hunk.theirs_label = label;
      out->hunks.push_back(hunk);
      where = kOutside;
    } else if (where == kOurs) {
      hunk.ours.push_back(line);
    } else if (where == kBase) {
      hunk.base.push_back(line);
    } else {
      hunk.theirs.push_back(line);
    }
  }
  out->unclosed = where != kOutside;
}

std::string ResolveConflictText(const model::ConflictFile& file, std::size_t hunk,
                                model::Side side) {
  const model::ConflictHunk& h = file.hunks[hunk];
  std::vector<std::string> lines(file.lines.begin(),
                                 file.lines.begin() + static_cast<std::ptrdiff_t>(h.begin));
  if (side != model::Side::Theirs) {
    lines.insert(lines.end(), h.ours.begin(), h.ours.end());
  }
  if (side != model::Side::Ours) {
    lines.insert(lines.end(), h.theirs.begin(), h.theirs.end());
  }
  lines.insert(lines.end(), file.lines.begin() + static_cast<std::ptrdiff_t>(h.end) + 1,
               file.lines.end());

  std::string text;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    text += lines[i];
    if (i + 1 < lines.size() || file.trailing_newline) {
      text += '\n';
    }
  }
  return text;
}

model::ConflictFile Repository::ReadConflict(const std::string& path) const {
  git_repository* repo = repo_.get();
  model::ConflictFile file;
  file.path = path;

  git_index* index = nullptr;
  const git_index_entry* ancestor = nullptr;
  const git_index_entry* ours = nullptr;
  const git_index_entry* theirs = nullptr;
  if (git_repository_index(&index, repo) != 0 ||
      git_index_conflict_get(&ancestor, &ours, &theirs, index, path.c_str()) != 0) {
    file.error = path + " is not conflicted";
    git_index_free(index);
    return file;
  }
  file.ours_exists = ours != nullptr;
  file.theirs_exists = theirs != nullptr;
  for (const git_index_entry* side : {ours, theirs}) {
    git_blob* blob = nullptr;
    if (side != nullptr && git_blob_lookup(&blob, repo, &side->id) == 0) {
      file.binary = file.binary || git_blob_is_binary(blob) == 1;
    }
    git_blob_free(blob);
  }
  git_index_free(index);

  if (!file.binary) {
    bool ok = false;
    const std::string text = ReadFile(std::filesystem::path(WorkdirPath()) / path, &ok);
    if (ok) {
      ParseConflictMarkers(text, &file);
    }
  }
  return file;
}

OpResult Repository::ResolveHunk(const std::string& path, std::size_t hunk, std::size_t begin,
                                 model::Side side) {
  const model::ConflictFile file = ReadConflict(path);
  if (!file.error.empty()) {
    return OpResult::Fail(file.error);
  }
  if (hunk >= file.hunks.size() || file.hunks[hunk].begin != begin) {
    return OpResult::Fail(path + " changed since it was read — look again");
  }
  if (!WriteFile(std::filesystem::path(WorkdirPath()) / path,
                 ResolveConflictText(file, hunk, side))) {
    return OpResult::Fail("could not write " + path);
  }
  const char* took = side == model::Side::Ours ? "ours" : side == model::Side::Theirs ? "theirs" : "both";
  const std::size_t left = file.hunks.size() - 1;
  if (left > 0) {
    return OpResult::Ok(std::string("took ") + took + " — " + Plural(left, "conflict") +
                        " left in " + path);
  }
  const OpResult staged = StageResolved(repo_.get(), path, false);
  if (!staged.ok) {
    return staged;
  }
  return OpResult::Ok(std::string("took ") + took + " — " + path + " resolved and staged");
}

OpResult Repository::ResolveFile(const std::string& path, model::Side side) {
  if (side == model::Side::Both) {
    return OpResult::Fail("keep both works a block at a time");
  }
  git_repository* repo = repo_.get();
  git_index* index = nullptr;
  const git_index_entry* ancestor = nullptr;
  const git_index_entry* ours = nullptr;
  const git_index_entry* theirs = nullptr;
  if (git_repository_index(&index, repo) != 0 ||
      git_index_conflict_get(&ancestor, &ours, &theirs, index, path.c_str()) != 0) {
    git_index_free(index);
    return OpResult::Fail(path + " is not conflicted");
  }
  const git_index_entry* chosen = side == model::Side::Ours ? ours : theirs;
  const char* name = side == model::Side::Ours ? "ours" : "theirs";
  const std::filesystem::path full = std::filesystem::path(WorkdirPath()) / path;

  // That side deleted the file, so taking it means deleting it here too.
  if (chosen == nullptr) {
    git_index_free(index);
    std::error_code ignored;
    std::filesystem::remove(full, ignored);
    const OpResult staged = StageResolved(repo, path, true);
    return staged.ok ? OpResult::Ok(std::string("took ") + name + " — " + path + " deleted")
                     : staged;
  }

  git_blob* blob = nullptr;
  const unsigned int mode = chosen->mode;
  const bool found = git_blob_lookup(&blob, repo, &chosen->id) == 0;
  git_index_free(index);
  if (!found) {
    return OpResult::Fail(LastError());
  }
  const std::string content(static_cast<const char*>(git_blob_rawcontent(blob)),
                            static_cast<std::size_t>(git_blob_rawsize(blob)));
  git_blob_free(blob);
  if (!WriteFile(full, content)) {
    return OpResult::Fail("could not write " + path);
  }
  if (mode == GIT_FILEMODE_BLOB_EXECUTABLE) {
    std::error_code ignored;
    std::filesystem::permissions(full,
                                 std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add, ignored);
  }
  const OpResult staged = StageResolved(repo, path, false);
  return staged.ok ? OpResult::Ok(std::string("took ") + name + " for all of " + path +
                                  " — resolved and staged")
                   : staged;
}

OpResult Repository::MarkResolved(const std::string& path) {
  const model::ConflictFile file = ReadConflict(path);
  if (!file.error.empty()) {
    return OpResult::Fail(file.error);
  }
  if (!file.hunks.empty()) {
    return OpResult::Fail(Plural(file.hunks.size(), "conflict") + " still marked in " + path);
  }
  if (file.unclosed) {
    return OpResult::Fail(path + " has a <<<<<<< with no >>>>>>> after it");
  }
  const bool gone = !std::filesystem::exists(std::filesystem::path(WorkdirPath()) / path);
  const OpResult staged = StageResolved(repo_.get(), path, gone);
  return staged.ok ? OpResult::Ok(path + " resolved and staged") : staged;
}

OpResult Repository::Merge(const model::Branch& branch) {
  git_repository* repo = repo_.get();
  if (branch.is_head) {
    return OpResult::Fail("cannot merge " + branch.name + " into itself");
  }
  if (const char* why = Blocked(*this, repo); why != nullptr) {
    return OpResult::Fail(why);
  }

  git_reference* ref = nullptr;
  if (git_branch_lookup(&ref, repo, branch.name.c_str(),
                        branch.remote ? GIT_BRANCH_REMOTE : GIT_BRANCH_LOCAL) != 0) {
    return OpResult::Fail(LastError());
  }
  git_annotated_commit* theirs = nullptr;
  if (git_annotated_commit_from_ref(&theirs, repo, ref) != 0) {
    git_reference_free(ref);
    return OpResult::Fail(LastError());
  }
  git_reference_free(ref);

  git_merge_analysis_t analysis{};
  git_merge_preference_t preference{};
  const git_annotated_commit* heads[] = {theirs};
  if (git_merge_analysis(&analysis, &preference, repo, heads, 1) != 0) {
    git_annotated_commit_free(theirs);
    return OpResult::Fail(LastError());
  }

  OpResult result = OpResult::Ok();
  git_checkout_options checkout;
  git_checkout_options_init(&checkout, GIT_CHECKOUT_OPTIONS_VERSION);
  checkout.checkout_strategy = GIT_CHECKOUT_SAFE;

  if ((analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE) != 0) {
    result = OpResult::Ok("already up to date with " + branch.name);
  } else if ((analysis & GIT_MERGE_ANALYSIS_UNBORN) != 0) {
    result = OpResult::Fail("nothing to merge into — make a first commit");
  } else if ((analysis & GIT_MERGE_ANALYSIS_FASTFORWARD) != 0 &&
             (preference & GIT_MERGE_PREFERENCE_NO_FASTFORWARD) == 0) {
    // HEAD has nothing the branch lacks, so the merge is moving the branch
    // forward, as `git merge` does unless merge.ff says not to.
    git_object* target = nullptr;
    git_reference* head = nullptr;
    git_reference* moved = nullptr;
    const std::string log = "merge " + branch.name + ": Fast-forward";
    if (git_object_lookup(&target, repo, git_annotated_commit_id(theirs), GIT_OBJECT_COMMIT) != 0 ||
        git_checkout_tree(repo, target, &checkout) != 0 || git_repository_head(&head, repo) != 0 ||
        git_reference_set_target(&moved, head, git_object_id(target), log.c_str()) != 0) {
      result = OpResult::Fail(LastError());
    } else {
      result = OpResult::Ok("fast-forwarded to " + branch.name);
    }
    git_reference_free(moved);
    git_reference_free(head);
    git_object_free(target);
  } else {
    git_merge_options options;
    git_merge_options_init(&options, GIT_MERGE_OPTIONS_VERSION);
    checkout.checkout_strategy = GIT_CHECKOUT_SAFE | GIT_CHECKOUT_ALLOW_CONFLICTS;
    if (git_merge(repo, heads, 1, &options, &checkout) != 0) {
      result = OpResult::Fail(LastError());
    } else if (const std::size_t conflicts = CountConflicts(repo); conflicts > 0) {
      result = OpResult::Ok("merge of " + branch.name + " stopped at " +
                            Plural(conflicts, "conflict") + " — enter on one to resolve");
    } else {
      // Clean: commit it the way `git merge` would, with the message libgit2
      // wrote to MERGE_MSG. Commit reads MERGE_HEAD for the second parent.
      git_buf clean = GIT_BUF_INIT;
      const std::string pending = PendingMessage();
      const std::string message =
          git_message_prettify(&clean, pending.c_str(), 1, '#') == 0 ? clean.ptr : pending;
      git_buf_dispose(&clean);
      result = Commit(message);
      if (result.ok) {
        result = OpResult::Ok("merged " + branch.name);
      }
    }
  }
  git_annotated_commit_free(theirs);
  return result;
}

}  // namespace gittop::git
