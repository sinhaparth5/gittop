#include "remote/token.hpp"

#include <cstdlib>
#include <string>
#include <vector>

namespace gittop::remote {
namespace {

using model::Provider;
using model::TokenSource;

// Returns the value only if the variable is set *and* non-empty. An exported
// but empty GITHUB_TOKEN is a real thing in CI, and treating it as present
// turns "anonymous, works fine" into "401, does not work".
bool ReadEnv(const char* name, std::string* out) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return false;
  }
  *out = value;
  return true;
}

std::vector<const char*> EnvNamesFor(Provider provider) {
  switch (provider) {
    case Provider::GitHub:
      return {"GITTOP_TOKEN", "GITHUB_TOKEN", "GH_TOKEN"};
    case Provider::GitLab:
      return {"GITTOP_TOKEN", "GITLAB_TOKEN", "CI_JOB_TOKEN"};
    case Provider::Unknown:
      break;
  }
  return {"GITTOP_TOKEN"};
}

}  // namespace

Token ResolveToken(const model::RemoteRef& ref, const config::Config& config,
                   const std::string& config_path) {
  Token token;

  for (const char* name : EnvNamesFor(ref.provider)) {
    if (ReadEnv(name, &token.value)) {
      token.source = TokenSource::Environment;
      token.origin = name;
      return token;
    }
  }

  if (!ref.host.empty()) {
    const std::string from_config = config.HostValue(ref.host, "token");
    if (!from_config.empty()) {
      token.value = from_config;
      token.source = TokenSource::ConfigFile;
      token.origin = config_path;
      return token;
    }
  }

  return token;  // anonymous, which is a supported way to run
}

}  // namespace gittop::remote
