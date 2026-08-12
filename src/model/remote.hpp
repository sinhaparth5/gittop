#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gittop::model {

enum class Provider { Unknown, GitHub, GitLab };

inline std::string ProviderName(Provider provider) {
  switch (provider) {
    case Provider::GitHub:
      return "GitHub";
    case Provider::GitLab:
      return "GitLab";
    case Provider::Unknown:
      break;
  }
  return "Unknown";
}

// A parsed `git remote`. The difference between github.com, gitlab.com and a
// self-hosted instance is settled once, here, so nothing downstream has to
// re-derive it from a URL. `ui/` reads this and never parses anything itself.
struct RemoteRef {
  std::string name;  // "origin"
  std::string url;   // exactly as configured; never rewritten, never displayed with credentials
  Provider provider = Provider::Unknown;

  std::string host;   // "github.com"
  std::string owner;  // "sinhaparth5" — on GitLab this can be "group/subgroup"
  std::string repo;   // "gittop"

  std::string api_base;  // "https://api.github.com"
  std::string web_url;   // "https://github.com/sinhaparth5/gittop"
  bool self_hosted = false;

  bool valid() const {
    return provider != Provider::Unknown && !owner.empty() && !repo.empty();
  }
  std::string full_name() const { return owner + "/" + repo; }
};

// Provider-neutral repository facts. GitHub and GitLab spell every one of these
// differently; the normalization happens in remote/client.cpp. Counts are -1
// when the provider did not report them, which is different from zero.
struct RepoInfo {
  std::string full_name;
  std::string description;
  std::string default_branch;
  std::string web_url;
  std::string visibility;  // "public" / "private" / "internal"

  int stars = -1;
  int forks = -1;
  int open_issues = -1;
  int watchers = -1;

  bool archived = false;
  std::int64_t last_activity = 0;  // unix seconds, 0 when unknown
};

// Both providers expose a request budget, with different header names and
// different semantics for the reset field. Normalized to: how many are left,
// out of how many, and when the window rolls over.
struct RateLimit {
  bool known = false;
  int limit = -1;
  int remaining = -1;
  std::int64_t reset = 0;  // unix seconds
};

enum class FetchState {
  Idle,      // nothing asked for yet
  Loading,   // a request is in flight
  Ready,     // info is populated
  Failed,    // error holds something worth reading
};

// How the token was found, for display. Never holds the token itself: the value
// lives in remote::Token and is not part of anything the UI can render.
//
// SignedIn is the one that only exists in memory: a token obtained by signing
// in this session and not written to disk, either because saving failed or
// because there was nowhere to save it to. It is a separate source rather than
// a flavour of ConfigFile so the panel can say the sign-in will not survive a
// restart, which is the one thing the user would otherwise find out the hard
// way — the next launch.
enum class TokenSource { None, Environment, ConfigFile, SignedIn };

struct RemoteSnapshot {
  FetchState state = FetchState::Idle;
  RemoteRef ref;
  RepoInfo info;
  RateLimit rate;

  TokenSource token_source = TokenSource::None;
  std::string token_origin;  // "GITHUB_TOKEN", or the config path — never the token

  std::string error;  // human-readable, safe to print
  std::string hint;   // what the user can do about it
  std::int64_t fetched_at = 0;

  bool authenticated() const { return token_source != TokenSource::None; }
};

}  // namespace gittop::model
