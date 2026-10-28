#include <doctest/doctest.h>

#include <string>

#include "remote/provider.hpp"

using gittop::model::Provider;
using gittop::remote::ParseRemote;

TEST_CASE("scp-style GitHub remote") {
  const auto ref = ParseRemote("origin", "git@github.com:sinhaparth5/gittop.git");
  CHECK(ref.provider == Provider::GitHub);
  CHECK(ref.host == "github.com");
  CHECK(ref.owner == "sinhaparth5");
  CHECK(ref.repo == "gittop");
}

TEST_CASE("GitLab subgroups stay in the owner, and credentials go nowhere") {
  const auto ref = ParseRemote("origin", "https://user:secret@gitlab.com/group/sub/proj.git");
  CHECK(ref.provider == Provider::GitLab);
  CHECK(ref.owner == "group/sub");
  CHECK(ref.repo == "proj");
  CHECK(ref.host.find("secret") == std::string::npos);
  CHECK(ref.web_url.find("secret") == std::string::npos);
  CHECK(ref.api_base.find("secret") == std::string::npos);
}

TEST_CASE("a local path is not a provider") {
  CHECK(ParseRemote("origin", "/srv/git/project.git").provider == Provider::Unknown);
}
