#include <doctest/doctest.h>

#include <git2.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "git/repository.hpp"
#include "scratch_dir.hpp"

using gittop::git::Repository;

namespace {

// A repository with one commit on `main`, an `origin` remote, and a
// remote-tracking branch `origin/feature` that no local branch tracks — the row
// the branches view used not to show at all.
std::optional<Repository> Fixture(const ScratchDir& dir) {
  git_repository* raw = nullptr;
  git_repository_init_options init;
  git_repository_init_options_init(&init, GIT_REPOSITORY_INIT_OPTIONS_VERSION);
  init.initial_head = "main";
  init.flags |= GIT_REPOSITORY_INIT_MKPATH;
  REQUIRE(git_repository_init_ext(&raw, dir.path().string().c_str(), &init) == 0);

  git_config* config = nullptr;
  REQUIRE(git_repository_config(&config, raw) == 0);
  git_config_set_string(config, "user.name", "Test");
  git_config_set_string(config, "user.email", "test@example.invalid");
  git_config_free(config);

  git_remote* remote = nullptr;
  REQUIRE(git_remote_create(&remote, raw, "origin", "https://example.invalid/x.git") == 0);
  git_remote_free(remote);
  git_repository_free(raw);

  std::ofstream(dir.path() / "README") << "hello\n";
  std::string error;
  std::optional<Repository> repo = Repository::Discover(dir.path().string(), &error);
  REQUIRE(repo.has_value());
  REQUIRE(repo->StageAll().ok);
  REQUIRE(repo->Commit("first").ok);

  REQUIRE(git_repository_open(&raw, dir.path().string().c_str()) == 0);
  git_reference* head = nullptr;
  REQUIRE(git_repository_head(&head, raw) == 0);
  git_reference* feature = nullptr;
  REQUIRE(git_reference_create(&feature, raw, "refs/remotes/origin/feature",
                               git_reference_target(head), 0, nullptr) == 0);
  git_reference_free(feature);
  git_reference_free(head);
  git_repository_free(raw);
  return repo;
}

}  // namespace

TEST_CASE("status reads an untracked file") {
  ScratchDir dir;
  std::optional<Repository> repo = Fixture(dir);
  std::ofstream(dir.path() / "new.txt") << "x\n";
  const auto status = repo->ReadStatus();
  CHECK(status.branch == "main");
  CHECK(status.untracked == 1);
}

TEST_CASE("a remote-only branch is listed, and checking it out tracks it") {
  ScratchDir dir;
  std::optional<Repository> repo = Fixture(dir);

  const auto history = repo->ReadHistory(50, 7);
  const gittop::model::Branch* remote_only = nullptr;
  for (const auto& branch : history.branches) {
    if (branch.name == "origin/feature") {
      remote_only = &branch;
    }
  }
  REQUIRE(remote_only != nullptr);
  CHECK(remote_only->remote);
  CHECK_FALSE(remote_only->has_upstream);
  CHECK(history.branches.front().name == "main");  // HEAD pinned above it

  REQUIRE(repo->Checkout(*remote_only).ok);
  const auto status = repo->ReadStatus();
  CHECK(status.branch == "feature");
  CHECK(status.upstream == "origin/feature");

  // Once tracked, it folds into the local row instead of appearing twice.
  for (const auto& branch : repo->ReadHistory(50, 7).branches) {
    CHECK_FALSE(branch.remote);
  }
}

namespace {

std::string Slurp(const std::filesystem::path& path) {
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

// The staged copy of README, which is what `git diff --cached` compares.
std::string IndexedReadme(const ScratchDir& dir) {
  git_repository* raw = nullptr;
  REQUIRE(git_repository_open(&raw, dir.path().string().c_str()) == 0);
  git_index* index = nullptr;
  REQUIRE(git_repository_index(&index, raw) == 0);
  const git_index_entry* entry = git_index_get_bypath(index, "README", 0);
  REQUIRE(entry != nullptr);
  git_blob* blob = nullptr;
  REQUIRE(git_blob_lookup(&blob, raw, &entry->id) == 0);
  std::string text(static_cast<const char*>(git_blob_rawcontent(blob)),
                   static_cast<std::size_t>(git_blob_rawsize(blob)));
  git_blob_free(blob);
  git_index_free(index);
  git_repository_free(raw);
  return text;
}

// Two hunks far enough apart that three lines of context cannot merge them.
std::string Numbered(const std::string& first, const std::string& last) {
  std::string text = first + "\n";
  for (int i = 2; i < 20; ++i) text += "line " + std::to_string(i) + "\n";
  return text + last + "\n";
}

gittop::git::HunkPick Pick(Repository& repo, gittop::model::DiffSource source, std::size_t hunk,
                           int line) {
  gittop::git::DiffRequest request;
  request.source = source;
  request.path = "README";
  const gittop::model::DiffSnapshot diff = repo.ReadDiff(request, 1000);
  gittop::git::HunkPick pick;
  pick.source = source;
  pick.path = "README";
  pick.hunk = hunk;
  pick.line = line;
  std::size_t seen = 0;
  for (const auto& row : diff.lines) {
    if (row.kind == gittop::model::DiffLineKind::HunkHeader && seen++ == hunk) {
      pick.header = row.text;
    }
  }
  return pick;
}

}  // namespace

TEST_CASE("one hunk stages and unstages without the other") {
  using gittop::model::DiffSource;
  ScratchDir dir;
  std::optional<Repository> repo = Fixture(dir);
  std::ofstream(dir.path() / "README") << Numbered("one", "twenty");
  REQUIRE(repo->StageAll().ok);
  REQUIRE(repo->Commit("numbered").ok);

  std::ofstream(dir.path() / "README") << Numbered("ONE", "TWENTY");
  REQUIRE(repo->ApplyHunk(Pick(*repo, DiffSource::Worktree, 1, -1)).ok);
  CHECK(IndexedReadme(dir) == Numbered("one", "TWENTY"));
  CHECK(Slurp(dir.path() / "README") == Numbered("ONE", "TWENTY"));

  REQUIRE(repo->ApplyHunk(Pick(*repo, DiffSource::Staged, 0, -1)).ok);
  CHECK(IndexedReadme(dir) == Numbered("one", "twenty"));
}

TEST_CASE("single added and removed lines stage on their own") {
  using gittop::model::DiffSource;
  ScratchDir dir;
  std::optional<Repository> repo = Fixture(dir);
  std::ofstream(dir.path() / "README") << "a\nb\nc\n";
  REQUIRE(repo->StageAll().ok);
  REQUIRE(repo->Commit("abc").ok);

  // Hunk lines: -a +A b -c +C
  std::ofstream(dir.path() / "README") << "A\nb\nC\n";
  REQUIRE(repo->ApplyHunk(Pick(*repo, DiffSource::Worktree, 0, 4)).ok);  // +C alone
  CHECK(IndexedReadme(dir) == "a\nb\nc\nC\n");
  REQUIRE(repo->ApplyHunk(Pick(*repo, DiffSource::Worktree, 0, 0)).ok);  // -a alone
  CHECK(IndexedReadme(dir) == "b\nc\nC\n");

  // Unstage the addition again, line by line from the staged side.
  // Staged hunk lines: -a b c +C
  CHECK_FALSE(repo->ApplyHunk(Pick(*repo, DiffSource::Staged, 0, 1)).ok);  // context
  REQUIRE(repo->ApplyHunk(Pick(*repo, DiffSource::Staged, 0, 3)).ok);
  CHECK(IndexedReadme(dir) == "b\nc\n");
}

TEST_CASE("blame follows a file across a rename") {
  ScratchDir dir;
  std::optional<Repository> repo = Fixture(dir);
  std::ofstream(dir.path() / "README") << "hello\nsecond\n";
  REQUIRE(repo->StageAll().ok);
  REQUIRE(repo->Commit("second line").ok);

  std::filesystem::rename(dir.path() / "README", dir.path() / "NOTES");
  REQUIRE(repo->StageAll().ok);
  REQUIRE(repo->Commit("rename").ok);

  const gittop::model::BlameSnapshot blame = repo->ReadBlame("NOTES");
  REQUIRE(blame.error.empty());
  REQUIRE(blame.lines.size() == 2);
  CHECK(blame.lines[0].summary == "first");
  CHECK(blame.lines[0].first);
  CHECK(blame.lines[1].summary == "second line");
  CHECK(blame.lines[1].text == "second");
  CHECK(blame.lines[1].author == "Test");
}

namespace {

using gittop::git::Rewrite;

// The first-parent line from HEAD, newest first, as full messages.
std::vector<std::string> Log(const ScratchDir& dir) {
  git_repository* raw = nullptr;
  REQUIRE(git_repository_open(&raw, dir.path().string().c_str()) == 0);
  std::vector<std::string> log;
  git_object* object = nullptr;
  REQUIRE(git_revparse_single(&object, raw, "HEAD") == 0);
  auto* commit = reinterpret_cast<git_commit*>(object);
  while (commit != nullptr) {
    log.emplace_back(git_commit_message(commit));
    git_commit* parent = nullptr;
    git_commit_parent(&parent, commit, 0);
    git_commit_free(commit);
    commit = parent;
  }
  git_repository_free(raw);
  return log;
}

std::string IdOf(const ScratchDir& dir, const char* spec) {
  git_repository* raw = nullptr;
  REQUIRE(git_repository_open(&raw, dir.path().string().c_str()) == 0);
  git_object* object = nullptr;
  REQUIRE(git_revparse_single(&object, raw, spec) == 0);
  std::string id = git_oid_tostr_s(git_object_id(object));
  git_object_free(object);
  git_repository_free(raw);
  return id;
}

void CommitFile(Repository& repo, const ScratchDir& dir, const char* file, const char* text,
                const char* message) {
  std::ofstream(dir.path() / file) << text;
  REQUIRE(repo.StageAll().ok);
  REQUIRE(repo.Commit(message).ok);
}

}  // namespace

TEST_CASE("squashing two commits leaves one with both changes and both messages") {
  ScratchDir dir;
  std::optional<Repository> repo = Fixture(dir);
  CommitFile(*repo, dir, "a.txt", "a\n", "add a");
  CommitFile(*repo, dir, "b.txt", "b\n", "add b");

  const gittop::git::RewriteScope scope = repo->PreviewRewrite(IdOf(dir, "HEAD"), Rewrite::Squash);
  CHECK(scope.error.empty());
  CHECK(scope.commits == 2);

  REQUIRE(repo->RewriteHistory(IdOf(dir, "HEAD"), Rewrite::Squash, "").ok);
  const std::vector<std::string> log = Log(dir);
  REQUIRE(log.size() == 2);
  CHECK(log[0] == "add a\n\nadd b");
  CHECK(Slurp(dir.path() / "a.txt") == "a\n");
  CHECK(Slurp(dir.path() / "b.txt") == "b\n");
  CHECK(repo->ReadOperation().operation == gittop::model::Operation::None);
}

TEST_CASE("fixup, reword, move and drop rewrite the line under HEAD") {
  ScratchDir dir;
  std::optional<Repository> repo = Fixture(dir);
  CommitFile(*repo, dir, "a.txt", "a\n", "add a");
  CommitFile(*repo, dir, "b.txt", "b\n", "add b");
  CommitFile(*repo, dir, "c.txt", "c\n", "add c");

  REQUIRE(repo->RewriteHistory(IdOf(dir, "HEAD~2"), Rewrite::MoveUp, "").ok);
  CHECK(Log(dir) == std::vector<std::string>{"add c", "add a", "add b", "first"});

  REQUIRE(repo->RewriteHistory(IdOf(dir, "HEAD~1"), Rewrite::Reword, "add a, renamed").ok);
  CHECK(Log(dir)[1] == "add a, renamed");

  REQUIRE(repo->RewriteHistory(IdOf(dir, "HEAD"), Rewrite::Fixup, "").ok);
  CHECK(Log(dir) == std::vector<std::string>{"add a, renamed", "add b", "first"});
  CHECK(Slurp(dir.path() / "c.txt") == "c\n");

  REQUIRE(repo->RewriteHistory(IdOf(dir, "HEAD~1"), Rewrite::Drop, "").ok);
  CHECK(Log(dir) == std::vector<std::string>{"add a, renamed", "first"});
  CHECK_FALSE(std::filesystem::exists(dir.path() / "b.txt"));

  CHECK_FALSE(repo->PreviewRewrite(IdOf(dir, "HEAD~1"), Rewrite::Drop).error.empty());
}

TEST_CASE("a conflict mid-rewrite stops as a rebase that continues") {
  ScratchDir dir;
  std::optional<Repository> repo = Fixture(dir);
  CommitFile(*repo, dir, "README", "one\n", "one");
  CommitFile(*repo, dir, "README", "two", "two");

  REQUIRE(repo->RewriteHistory(IdOf(dir, "HEAD~1"), Rewrite::Drop, "").ok);
  CHECK(repo->ReadOperation().operation == gittop::model::Operation::Rebase);
  CHECK_FALSE(repo->RebaseContinue().ok);  // still conflicted

  std::ofstream(dir.path() / "README") << "two";
  REQUIRE(repo->StageAll().ok);
  REQUIRE(repo->RebaseContinue().ok);
  CHECK(repo->ReadOperation().operation == gittop::model::Operation::None);
  CHECK(Log(dir) == std::vector<std::string>{"two", "first"});
}

TEST_CASE("an amend with nothing staged only rewrites the message") {
  ScratchDir dir;
  std::optional<Repository> repo = Fixture(dir);
  const std::string before = IdOf(dir, "HEAD^{tree}");
  bool pushed = true;
  CHECK(repo->HeadSummary(&pushed) == "first");
  CHECK_FALSE(pushed);  // origin/main does not exist, so nothing is upstream

  REQUIRE(repo->Amend("first, amended").ok);
  CHECK(Log(dir) == std::vector<std::string>{"first, amended"});
  CHECK(IdOf(dir, "HEAD^{tree}") == before);

  std::ofstream(dir.path() / "README") << "changed\n";
  REQUIRE(repo->StageAll().ok);
  REQUIRE(repo->Amend("first, amended").ok);
  CHECK(Log(dir).size() == 1);
  CHECK(IdOf(dir, "HEAD^{tree}") != before);
}

TEST_CASE("an edited message commits without comments, and an empty one aborts") {
  ScratchDir dir;
  std::optional<Repository> repo = Fixture(dir);
  std::ofstream(dir.path() / "a.txt") << "a\n";
  REQUIRE(repo->StageAll().ok);

  const std::string path = repo->PrepareCommitMessage();
  const gittop::git::OpResult empty = repo->CommitEditedMessage();
  CHECK_FALSE(empty.ok);
  CHECK(Log(dir).size() == 1);

  std::ofstream(path) << "add a\n\nwith a body\n# a comment\n";
  REQUIRE(repo->CommitEditedMessage().ok);
  CHECK(Log(dir)[0] == "add a\n\nwith a body\n");
}

TEST_CASE("branches are created, renamed and deleted, and an unmerged one says so") {
  ScratchDir dir;
  std::optional<Repository> repo = Fixture(dir);
  REQUIRE(repo->CreateBranch("topic", "main").ok);
  CHECK_FALSE(repo->CreateBranch("topic", "main").ok);  // taken
  CHECK_FALSE(repo->CreateBranch("bad name", "main").ok);
  CHECK(repo->BranchMerged("topic"));

  REQUIRE(repo->RenameBranch("topic", "feature").ok);
  CHECK(IdOf(dir, "feature") == IdOf(dir, "main"));
  CHECK_FALSE(repo->DeleteBranch("main").ok);  // checked out

  // A commit only `feature` has, made by moving the ref onto a child of main.
  CommitFile(*repo, dir, "a.txt", "a\n", "only on feature");
  REQUIRE(repo->CreateBranch("ahead", "HEAD").ok);
  REQUIRE(repo->RewriteHistory(IdOf(dir, "HEAD"), Rewrite::Drop, "").ok);
  CHECK_FALSE(repo->BranchMerged("ahead"));
  REQUIRE(repo->DeleteBranch("ahead").ok);
  REQUIRE(repo->DeleteBranch("feature").ok);
}

TEST_CASE("a tag is lightweight without a message and annotated with one") {
  ScratchDir dir;
  std::optional<Repository> repo = Fixture(dir);
  REQUIRE(repo->CreateTag(IdOf(dir, "HEAD"), "v1", "").ok);
  REQUIRE(repo->CreateTag(IdOf(dir, "HEAD"), "v2", "the second").ok);
  CHECK_FALSE(repo->CreateTag(IdOf(dir, "HEAD"), "v1", "").ok);  // never forced

  CHECK(IdOf(dir, "refs/tags/v1") == IdOf(dir, "HEAD"));  // points straight at it
  CHECK(IdOf(dir, "refs/tags/v2") != IdOf(dir, "HEAD"));  // a tag object
  CHECK(IdOf(dir, "v2^{commit}") == IdOf(dir, "HEAD"));
}
