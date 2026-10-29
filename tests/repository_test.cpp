#include <doctest/doctest.h>

#include <git2.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "git/repository.hpp"
#include "git/transfer.hpp"
#include "scratch_dir.hpp"

using gittop::git::Repository;

namespace {

// A repository with one commit on `main`, an `origin` remote, and a
// remote-tracking branch `origin/feature` that no local branch tracks — the row
// the branches view used not to show at all.
std::optional<Repository> Fixture(const std::filesystem::path& root) {
  git_repository* raw = nullptr;
  git_repository_init_options init;
  git_repository_init_options_init(&init, GIT_REPOSITORY_INIT_OPTIONS_VERSION);
  init.initial_head = "main";
  init.flags |= GIT_REPOSITORY_INIT_MKPATH;
  REQUIRE(git_repository_init_ext(&raw, root.string().c_str(), &init) == 0);

  git_config* config = nullptr;
  REQUIRE(git_repository_config(&config, raw) == 0);
  git_config_set_string(config, "user.name", "Test");
  git_config_set_string(config, "user.email", "test@example.invalid");
  git_config_free(config);

  git_remote* remote = nullptr;
  REQUIRE(git_remote_create(&remote, raw, "origin", "https://example.invalid/x.git") == 0);
  git_remote_free(remote);
  git_repository_free(raw);

  std::ofstream(root / "README") << "hello\n";
  std::string error;
  std::optional<Repository> repo = Repository::Discover(root.string(), &error);
  REQUIRE(repo.has_value());
  REQUIRE(repo->StageAll().ok);
  REQUIRE(repo->Commit("first").ok);

  REQUIRE(git_repository_open(&raw, root.string().c_str()) == 0);
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

std::optional<Repository> Fixture(const ScratchDir& dir) {
  return Fixture(dir.path());
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

TEST_CASE("cherry-picked commits land on HEAD in order, keeping their messages") {
  ScratchDir dir;
  std::optional<Repository> repo = Fixture(dir);
  CommitFile(*repo, dir, "a.txt", "a\n", "add a");
  CommitFile(*repo, dir, "b.txt", "b\n", "add b");
  const std::string a = IdOf(dir, "HEAD~1");
  const std::string b = IdOf(dir, "HEAD");
  REQUIRE(repo->RewriteHistory(a, Rewrite::Drop, "").ok);
  REQUIRE(repo->RewriteHistory(IdOf(dir, "HEAD"), Rewrite::Drop, "").ok);
  REQUIRE(Log(dir).size() == 1);

  std::size_t applied = 0;
  REQUIRE(repo->CherryPick({a, b}, &applied).ok);
  CHECK(applied == 2);
  CHECK(Log(dir) == std::vector<std::string>{"add b", "add a", "first"});
  CHECK(repo->ReadOperation().operation == gittop::model::Operation::None);
}

TEST_CASE("a conflicting cherry-pick stops, and committing finishes it") {
  ScratchDir dir;
  std::optional<Repository> repo = Fixture(dir);
  CommitFile(*repo, dir, "README", "theirs\n", "theirs");
  const std::string theirs = IdOf(dir, "HEAD");
  REQUIRE(repo->RewriteHistory(theirs, Rewrite::Drop, "").ok);
  CommitFile(*repo, dir, "README", "ours\n", "ours");

  std::size_t applied = 0;
  REQUIRE(repo->CherryPick({theirs}, &applied).ok);
  CHECK(applied == 0);
  CHECK(repo->ReadOperation().operation == gittop::model::Operation::CherryPick);
  CHECK(repo->PendingMessage().rfind("theirs", 0) == 0);

  std::ofstream(dir.path() / "README") << "both\n";
  REQUIRE(repo->StageAll().ok);
  REQUIRE(repo->Commit("theirs").ok);
  CHECK(repo->ReadOperation().operation == gittop::model::Operation::None);
}

TEST_CASE("a revert commits the inverse of the change") {
  ScratchDir dir;
  std::optional<Repository> repo = Fixture(dir);
  CommitFile(*repo, dir, "a.txt", "a\n", "add a");
  REQUIRE(repo->Revert(IdOf(dir, "HEAD")).ok);
  CHECK_FALSE(std::filesystem::exists(dir.path() / "a.txt"));
  CHECK(Log(dir)[0].rfind("Revert \"add a\"\n\nThis reverts commit ", 0) == 0);
}

TEST_CASE("one undo takes back a whole rewrite, and refuses on a dirty tree") {
  ScratchDir dir;
  std::optional<Repository> repo = Fixture(dir);
  CommitFile(*repo, dir, "a.txt", "a\n", "add a");
  CommitFile(*repo, dir, "b.txt", "b\n", "add b");
  CommitFile(*repo, dir, "c.txt", "c\n", "add c");
  const std::vector<std::string> before = Log(dir);

  REQUIRE(repo->RewriteHistory(IdOf(dir, "HEAD~1"), Rewrite::Drop, "").ok);
  REQUIRE(Log(dir).size() == before.size() - 1);

  const std::vector<gittop::model::ReflogEntry> reflog = repo->ReadReflog(50);
  const std::size_t steps = gittop::git::UndoSteps(reflog);
  REQUIRE(steps > 1);
  CHECK(reflog[0].id == IdOf(dir, "HEAD"));

  std::ofstream(dir.path() / "a.txt") << "dirty\n";
  CHECK_FALSE(repo->ResetTo(reflog[steps - 1].old_id).ok);
  std::ofstream(dir.path() / "a.txt") << "a\n";

  REQUIRE(repo->ResetTo(reflog[steps - 1].old_id).ok);
  CHECK(Log(dir) == before);
  CHECK(Slurp(dir.path() / "b.txt") == "b\n");
  // The reset is itself an entry, so a second undo puts the rewrite back.
  CHECK(gittop::git::UndoSteps(repo->ReadReflog(50)) == 1);
}

TEST_CASE("an undo outside a rebase is one entry") {
  const auto log = [](std::initializer_list<const char*> messages) {
    std::vector<gittop::model::ReflogEntry> out;
    for (const char* message : messages) {
      out.emplace_back().message = message;
    }
    return out;
  };
  CHECK(gittop::git::UndoSteps({}) == 0);
  CHECK(gittop::git::UndoSteps(log({"commit (amend): x", "rebase: y"})) == 1);
  CHECK(gittop::git::UndoSteps(log({"rebase (finish): returning", "rebase (pick): b",
                                    "rebase (start): checkout main",
                                    "rebase (finish): an older one"})) == 3);
}

// Built in a subdirectory, because a worktree goes beside the main tree and
// beside the scratch directory itself would outlive the test.
TEST_CASE("a worktree is added beside the main tree, listed, and removed") {
  ScratchDir dir;
  std::optional<Repository> repo = Fixture(dir.path() / "proj");
  const std::filesystem::path linked_path = dir.path() / "proj-topic";

  REQUIRE(repo->AddWorktree("topic").ok);
  CHECK(std::filesystem::exists(linked_path / "README"));
  CHECK_FALSE(repo->AddWorktree("topic").ok);

  const gittop::model::Workspace workspace = repo->ReadWorkspace();
  REQUIRE(workspace.worktrees.size() == 2);
  CHECK(workspace.worktrees[0].main);
  CHECK(workspace.worktrees[0].current);
  CHECK(workspace.worktrees[0].branch == "main");
  CHECK(workspace.worktrees[1].name == "topic");
  CHECK(workspace.worktrees[1].branch == "topic");
  CHECK(workspace.worktrees[1].changes == 0);

  // From inside the linked one, the main tree is still first and not current.
  std::string error;
  std::optional<Repository> linked = Repository::Discover(linked_path.string(), &error);
  REQUIRE(linked.has_value());
  const gittop::model::Workspace seen = linked->ReadWorkspace();
  REQUIRE(seen.worktrees.size() == 2);
  CHECK(seen.worktrees[0].main);
  CHECK_FALSE(seen.worktrees[0].current);
  CHECK(seen.worktrees[1].current);

  std::ofstream(linked_path / "x.txt") << "x\n";
  CHECK_FALSE(repo->RemoveWorktree("topic").ok);
  std::filesystem::remove(linked_path / "x.txt");
  REQUIRE(repo->RemoveWorktree("topic").ok);
  CHECK_FALSE(std::filesystem::exists(linked_path));
  CHECK(repo->ReadWorkspace().worktrees.size() == 1);
}

TEST_CASE("a worktree whose directory is gone is listed as missing, and removing it prunes") {
  ScratchDir dir;
  std::optional<Repository> repo = Fixture(dir.path() / "proj");
  REQUIRE(repo->AddWorktree("gone").ok);
  std::filesystem::remove_all(dir.path() / "proj-gone");

  const gittop::model::Workspace workspace = repo->ReadWorkspace();
  REQUIRE(workspace.worktrees.size() == 2);
  CHECK(workspace.worktrees[1].missing);
  CHECK(repo->RemoveWorktree("gone").ok);
  CHECK(repo->ReadWorkspace().worktrees.size() == 1);
}

TEST_CASE("a submodule is listed, and an update clones it at the recorded commit") {
  ScratchDir dir;
  std::optional<Repository> lib = Fixture(dir.path() / "lib");
  std::optional<Repository> proj = Fixture(dir.path() / "proj");

  git_repository* raw = nullptr;
  REQUIRE(git_repository_open(&raw, (dir.path() / "proj").string().c_str()) == 0);
  git_submodule* module = nullptr;
  REQUIRE(git_submodule_add_setup(&module, raw, (dir.path() / "lib").string().c_str(),
                                  "vendor/lib", 1) == 0);
  REQUIRE(git_submodule_clone(nullptr, module, nullptr) == 0);
  REQUIRE(git_submodule_add_finalize(module) == 0);
  git_submodule_free(module);
  git_repository_free(raw);
  // The status read is what picks up an index another handle wrote, as App's
  // refresh does.
  proj->ReadStatus();
  REQUIRE(proj->Commit("add lib").ok);

  gittop::model::Workspace workspace = proj->ReadWorkspace();
  REQUIRE(workspace.submodules.size() == 1);
  CHECK(workspace.submodules[0].path == "vendor/lib");
  CHECK(workspace.submodules[0].cloned);
  CHECK(workspace.submodules[0].recorded == workspace.submodules[0].checked_out);

  // A clone of the superproject has the submodule recorded and nothing in it.
  git_repository* clone = nullptr;
  const std::filesystem::path copy = dir.path() / "copy";
  REQUIRE(git_clone(&clone, (dir.path() / "proj").string().c_str(), copy.string().c_str(),
                    nullptr) == 0);
  git_repository_free(clone);
  std::string error;
  std::optional<Repository> copied = Repository::Discover(copy.string(), &error);
  REQUIRE(copied.has_value());
  CHECK_FALSE(copied->ReadWorkspace().submodules.at(0).cloned);

  const gittop::git::TransferResult result =
      gittop::git::UpdateSubmodule(copy.string(), "vendor/lib", nullptr, nullptr);
  CHECK_MESSAGE(result.ok, result.detail);
  CHECK(std::filesystem::exists(copy / "vendor/lib/README"));
  workspace = copied->ReadWorkspace();
  CHECK(workspace.submodules.at(0).cloned);
  CHECK(workspace.submodules.at(0).checked_out == workspace.submodules.at(0).recorded);
}

namespace {

gittop::model::Branch LocalBranch(const char* name) {
  gittop::model::Branch branch;
  branch.name = name;
  return branch;
}

std::string Slurp(const ScratchDir& dir, const char* file) {
  std::ifstream in(dir.path() / file, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// main and topic both rewrite README from "hello", so merging topic stops.
void Diverge(Repository& repo, const ScratchDir& dir) {
  REQUIRE(repo.CreateBranch("topic", "HEAD").ok);
  REQUIRE(repo.Checkout(LocalBranch("topic")).ok);
  CommitFile(repo, dir, "README", "topic\n", "on topic");
  REQUIRE(repo.Checkout(LocalBranch("main")).ok);
  CommitFile(repo, dir, "README", "main\n", "on main");
}

}  // namespace

TEST_CASE("conflict markers parse two-way and diff3, and skip an unclosed block") {
  gittop::model::ConflictFile file;
  gittop::git::ParseConflictMarkers(
      "keep\n<<<<<<< HEAD\nours\n=======\ntheirs\n>>>>>>> topic\nmid\n"
      "<<<<<<< HEAD\r\na\r\n||||||| base\r\nb\r\n=======\r\nc\r\n>>>>>>> x\r\n"
      "<<<<<<< HEAD\nnever closed\n",
      &file);
  REQUIRE(file.hunks.size() == 2);
  CHECK(file.hunks[0].ours_label == "HEAD");
  CHECK(file.hunks[0].theirs_label == "topic");
  CHECK(file.hunks[0].ours == std::vector<std::string>{"ours"});
  CHECK(file.hunks[0].theirs == std::vector<std::string>{"theirs"});
  CHECK(file.hunks[1].ours == std::vector<std::string>{"a\r"});
  CHECK(file.hunks[1].base == std::vector<std::string>{"b\r"});
  CHECK(file.hunks[1].theirs == std::vector<std::string>{"c\r"});
  CHECK(file.unclosed);

  CHECK(gittop::git::ResolveConflictText(file, 0, gittop::model::Side::Both).rfind(
            "keep\nours\ntheirs\nmid\n<<<<<<< HEAD\r\n", 0) == 0);
}

TEST_CASE("a conflicting merge stops, and resolving a block stages the file") {
  ScratchDir dir;
  std::optional<Repository> repo = Fixture(dir);
  Diverge(*repo, dir);

  REQUIRE(repo->Merge(LocalBranch("topic")).ok);
  CHECK(repo->ReadOperation().operation == gittop::model::Operation::Merge);
  const gittop::model::ConflictFile file = repo->ReadConflict("README");
  REQUIRE(file.hunks.size() == 1);
  CHECK(file.hunks[0].ours == std::vector<std::string>{"main"});
  CHECK(file.hunks[0].theirs == std::vector<std::string>{"topic"});

  // A stale view of the file is refused rather than applied to the wrong block.
  CHECK_FALSE(repo->ResolveHunk("README", 0, file.hunks[0].begin + 1,
                                gittop::model::Side::Ours).ok);
  REQUIRE(repo->ResolveHunk("README", 0, file.hunks[0].begin, gittop::model::Side::Both).ok);
  CHECK(Slurp(dir, "README") == "main\ntopic\n");
  CHECK(repo->ReadStatus().conflicted == 0);

  REQUIRE(repo->Commit("merge topic").ok);
  CHECK(repo->ReadOperation().operation == gittop::model::Operation::None);
  CHECK(Log(dir)[0] == "merge topic");
}

TEST_CASE("taking one side of a whole file, and marking an edited file resolved") {
  ScratchDir dir;
  std::optional<Repository> repo = Fixture(dir);
  Diverge(*repo, dir);
  REQUIRE(repo->Merge(LocalBranch("topic")).ok);

  CHECK_FALSE(repo->MarkResolved("README").ok);  // markers still in it
  std::ofstream(dir.path() / "README") << "half\n<<<<<<< HEAD\nedited\n";
  CHECK_FALSE(repo->MarkResolved("README").ok);  // an unclosed block is not clean
  REQUIRE(repo->ResolveFile("README", gittop::model::Side::Theirs).ok);
  CHECK(Slurp(dir, "README") == "topic\n");
  CHECK(repo->ReadStatus().conflicted == 0);
}

TEST_CASE("merge fast-forwards when behind, and commits a clean merge") {
  ScratchDir dir;
  std::optional<Repository> repo = Fixture(dir);
  REQUIRE(repo->CreateBranch("topic", "HEAD").ok);
  REQUIRE(repo->Checkout(LocalBranch("topic")).ok);
  CommitFile(*repo, dir, "a.txt", "a\n", "add a");
  REQUIRE(repo->Checkout(LocalBranch("main")).ok);

  REQUIRE(repo->Merge(LocalBranch("topic")).ok);
  CHECK(Log(dir) == std::vector<std::string>{"add a", "first"});
  CHECK(repo->Merge(LocalBranch("topic")).message.rfind("already up to date", 0) == 0);

  REQUIRE(repo->Checkout(LocalBranch("topic")).ok);
  CommitFile(*repo, dir, "b.txt", "b\n", "add b");
  REQUIRE(repo->Checkout(LocalBranch("main")).ok);
  CommitFile(*repo, dir, "c.txt", "c\n", "add c");
  REQUIRE(repo->Merge(LocalBranch("topic")).ok);
  CHECK(Log(dir)[0].rfind("Merge branch 'topic'", 0) == 0);
  CHECK(std::filesystem::exists(dir.path() / "b.txt"));
  CHECK(repo->ReadOperation().operation == gittop::model::Operation::None);
}

TEST_CASE("a file deleted on one side resolves whole, by deleting or keeping it") {
  ScratchDir dir;
  std::optional<Repository> repo = Fixture(dir);
  REQUIRE(repo->CreateBranch("topic", "HEAD").ok);
  REQUIRE(repo->Checkout(LocalBranch("topic")).ok);
  CommitFile(*repo, dir, "README", "changed on topic\n", "change");
  REQUIRE(repo->Checkout(LocalBranch("main")).ok);
  std::filesystem::remove(dir.path() / "README");
  REQUIRE(repo->StageAll().ok);
  REQUIRE(repo->Commit("delete").ok);

  REQUIRE(repo->Merge(LocalBranch("topic")).ok);
  const gittop::model::ConflictFile file = repo->ReadConflict("README");
  CHECK_FALSE(file.ours_exists);
  CHECK(file.theirs_exists);
  CHECK(file.hunks.empty());

  REQUIRE(repo->ResolveFile("README", gittop::model::Side::Ours).ok);
  CHECK_FALSE(std::filesystem::exists(dir.path() / "README"));
  CHECK(repo->ReadStatus().conflicted == 0);
  REQUIRE(repo->Commit("merge topic").ok);
  CHECK(repo->ReadOperation().operation == gittop::model::Operation::None);
}
