#include <git2.h>

#include <cstddef>
#include <string>
#include <unordered_map>

#include "git/internal.hpp"
#include "git/repository.hpp"

namespace gittop::git {

model::BlameSnapshot Repository::ReadBlame(const std::string& path) const {
  model::BlameSnapshot snap;
  snap.path = path;
  git_repository* repo = repo_.get();

  // HEAD's copy rather than the working tree's: a line nobody has committed has
  // no commit to name, and blaming a buffer would make up a row for it.
  git_object* blob_object = nullptr;
  if (git_revparse_single(&blob_object, repo, ("HEAD:" + path).c_str()) != 0) {
    snap.error = path + " is not in HEAD — nothing to blame yet";
    return snap;
  }
  git_blob* blob = reinterpret_cast<git_blob*>(blob_object);
  if (git_blob_is_binary(blob)) {
    git_object_free(blob_object);
    snap.error = path + " is binary";
    return snap;
  }
  const std::string content(static_cast<const char*>(git_blob_rawcontent(blob)),
                            static_cast<std::size_t>(git_blob_rawsize(blob)));
  git_object_free(blob_object);

  git_blame* blame = nullptr;
  if (git_blame_file(&blame, repo, path.c_str(), nullptr) != 0) {
    snap.error = LastError();
    return snap;
  }

  // One lookup per commit, not per hunk: a file touched by a handful of commits
  // breaks into far more hunks than that.
  std::unordered_map<std::string, std::string> summaries;
  std::size_t pos = 0;
  std::string last_id;
  for (int lineno = 1; pos < content.size(); ++lineno) {
    std::size_t end = content.find('\n', pos);
    if (end == std::string::npos) end = content.size();
    model::BlameLine line;
    line.lineno = lineno;
    line.text = content.substr(pos, end - pos);
    if (!line.text.empty() && line.text.back() == '\r') line.text.pop_back();
    pos = end + 1;

    const git_blame_hunk* hunk =
        git_blame_get_hunk_byline(blame, static_cast<std::size_t>(lineno));
    if (hunk != nullptr) {
      line.id = git_oid_tostr_s(&hunk->final_commit_id);
      if (hunk->final_signature != nullptr) {
        line.author = hunk->final_signature->name;
        line.time = hunk->final_signature->when.time;
      }
      auto [it, fresh] = summaries.try_emplace(line.id);
      if (fresh) {
        git_commit* commit = nullptr;
        if (git_commit_lookup(&commit, repo, &hunk->final_commit_id) == 0) {
          const char* summary = git_commit_summary(commit);
          it->second = summary != nullptr ? summary : "";
          git_commit_free(commit);
        }
      }
      line.summary = it->second;
    }
    line.first = line.id != last_id;
    last_id = line.id;
    snap.lines.push_back(std::move(line));
  }
  git_blame_free(blame);
  return snap;
}

}  // namespace gittop::git
