#pragma once

#include <string>

#include "config/config.hpp"
#include "model/remote.hpp"

namespace gittop::remote {

// The secret, kept in its own type so it is obvious at every call site what is
// being passed around. `origin` names where it came from and is safe to print;
// `value` never is, and nothing in ui/ takes one of these.
struct Token {
  std::string value;
  model::TokenSource source = model::TokenSource::None;
  std::string origin;

  bool present() const { return !value.empty(); }
};

// A token obtained by signing in during this session and held only in memory.
// It is scoped to a host because signing in to github.com says nothing about
// what may be sent to a self-hosted GitLab, and a token offered to the wrong
// host is a leaked token rather than a failed request.
struct SessionToken {
  std::string host;
  std::string value;

  bool MatchesHost(const std::string& other) const {
    return !host.empty() && !value.empty() && host == other;
  }
};

// Environment first, this session's sign-in second, config file third,
// unauthenticated fourth.
//
// The env var winning matters: it means CI and a throwaway shell never have to
// write a secret to disk, and it means overriding a stale config file is one
// `GITHUB_TOKEN=... gittop` away.
//
// The sign-in sitting above the config file rather than below it is what makes
// signing in work when the config already holds an expired token — the case
// where a user is most likely to be reaching for the sign-in in the first
// place. It stays below the environment so that ordering above is untouched.
//
// `config_path` is only used to say where a config-file token came from.
Token ResolveToken(const model::RemoteRef& ref, const config::Config& config,
                   const std::string& config_path, const SessionToken* session = nullptr);

}  // namespace gittop::remote
