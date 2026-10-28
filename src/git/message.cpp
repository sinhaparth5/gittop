#include <git2.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "git/internal.hpp"
#include "git/repository.hpp"

// Commit messages longer than one line: the editor, and amending HEAD. The
// commit box is a single-line Input and stays one; a message with a body is
// what an editor is for, and gittop hands the terminal over rather than
// growing a text editor of its own.

namespace gittop::git {
namespace {

std::filesystem::path MessagePath(git_repository* repo) {
  return std::filesystem::path(git_repository_path(repo)) / "COMMIT_EDITMSG";
}

void HeadIfPushed(git_repository* repo, const git_oid& head, bool* pushed) {
  *pushed = false;
  git_reference* ref = nullptr;
  git_reference* upstream = nullptr;
  if (git_repository_head(&ref, repo) == 0 && git_branch_upstream(&upstream, ref) == 0) {
    const git_oid* up = git_reference_target(upstream);
    *pushed = up != nullptr &&
              (git_oid_equal(up, &head) || git_graph_descendant_of(repo, up, &head) == 1);
  }
  git_reference_free(upstream);
  git_reference_free(ref);
}

}  // namespace

std::vector<std::string> Repository::EditorCommand() const {
  // git's own order. core.editor sits between the two variables because a
  // repository can want a different editor than the shell does.
  std::string command;
  if (const char* env = std::getenv("GIT_EDITOR"); env != nullptr && *env != '\0') {
    command = env;
  }
  git_config* config = nullptr;
  git_buf value = GIT_BUF_INIT;
  if (command.empty() && git_repository_config(&config, repo_.get()) == 0 &&
      git_config_get_string_buf(&value, config, "core.editor") == 0) {
    command = value.ptr;
  }
  git_buf_dispose(&value);
  git_config_free(config);
  for (const char* name : {"VISUAL", "EDITOR"}) {
    if (const char* env = std::getenv(name); command.empty() && env != nullptr && *env != '\0') {
      command = env;
    }
  }
  if (command.empty()) {
    command = "vi";
  }

  // ponytail: split on whitespace, so "code --wait" works and a quoted path
  // with a space in it does not. git hands this string to a shell, which is
  // the one thing this must not do with a value read from the environment.
  std::istringstream words(command);
  return {std::istream_iterator<std::string>(words), std::istream_iterator<std::string>()};
}

std::string Repository::PrepareCommitMessage() const {
  const std::filesystem::path path = MessagePath(repo_.get());
  std::ofstream(path) << "\n"
                         "# Write the message for this commit above. Lines starting with '#'\n"
                         "# are ignored, and an empty message aborts the commit.\n";
  return path.string();
}

OpResult Repository::CommitEditedMessage() {
  std::ifstream in(MessagePath(repo_.get()));
  const std::string raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  // Strips the comments and the blank lines around the text, which is exactly
  // what `git commit` does to the same file.
  git_buf clean = GIT_BUF_INIT;
  if (git_message_prettify(&clean, raw.c_str(), 1, '#') != 0) {
    return OpResult::Fail(LastError());
  }
  const std::string message = clean.ptr;
  git_buf_dispose(&clean);
  if (message.empty()) {
    return OpResult::Fail("empty message — commit aborted");
  }
  return Commit(message);
}

std::string Repository::HeadSummary(bool* pushed) const {
  git_repository* repo = const_cast<git_repository*>(repo_.get());
  git_oid head{};
  git_commit* commit = nullptr;
  if (git_reference_name_to_id(&head, repo, "HEAD") != 0 ||
      git_commit_lookup(&commit, repo, &head) != 0) {
    *pushed = false;
    return {};
  }
  HeadIfPushed(repo, head, pushed);
  const char* summary = git_commit_summary(commit);
  std::string out = summary != nullptr ? summary : "";
  git_commit_free(commit);
  return out;
}

OpResult Repository::Amend(const std::string& summary) {
  git_repository* repo = repo_.get();
  if (summary.empty()) {
    return OpResult::Fail("commit message is empty");
  }
  // Mid-merge, mid-rebase or mid-anything, HEAD is not the commit the user
  // thinks of as the last one, and amending it is how that state gets worse.
  if (git_repository_state(repo) != GIT_REPOSITORY_STATE_NONE) {
    return OpResult::Fail("finish or abort what is in progress first");
  }

  git_index* index = nullptr;
  git_oid tree_id{};
  git_tree* tree = nullptr;
  git_object* head = nullptr;
  git_signature* who = nullptr;
  OpResult result = OpResult::Fail("");
  if (git_repository_index(&index, repo) != 0 || git_revparse_single(&head, repo, "HEAD") != 0) {
    result = OpResult::Fail("there is no commit to amend yet");
  } else if (git_index_has_conflicts(index) == 1) {
    result = OpResult::Fail("resolve conflicts before amending");
  } else if (git_signature_default(&who, repo) != 0) {
    result = OpResult::Fail("set user.name and user.email before committing");
  } else if (git_index_write_tree(&tree_id, index) != 0 ||
             git_tree_lookup(&tree, repo, &tree_id) != 0) {
    result = OpResult::Fail(LastError());
  } else {
    // The summary is the one line the box can edit; the body comes along.
    auto* commit = reinterpret_cast<git_commit*>(head);
    std::string message = summary;
    if (const char* body = git_commit_body(commit); body != nullptr) {
      message += std::string("\n\n") + body;
    }
    git_oid id{};
    // The author stays; the committer is whoever amended it, as git does.
    if (git_commit_amend(&id, commit, "HEAD", nullptr, who, nullptr, message.c_str(), tree) != 0) {
      result = OpResult::Fail(LastError());
    } else {
      char short_id[8] = {};
      git_oid_tostr(short_id, sizeof(short_id), &id);
      result = OpResult::Ok(std::string("amended ") + short_id);
    }
  }
  git_signature_free(who);
  git_tree_free(tree);
  git_object_free(head);
  git_index_free(index);
  return result;
}

}  // namespace gittop::git
