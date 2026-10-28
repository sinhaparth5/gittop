#include <doctest/doctest.h>

#include <git2.h>

#include <fstream>
#include <optional>
#include <string>

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
