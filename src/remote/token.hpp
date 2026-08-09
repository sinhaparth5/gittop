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

// Environment first, config file second, unauthenticated third.
//
// The env var winning matters: it means CI and a throwaway shell never have to
// write a secret to disk, and it means overriding a stale config file is one
// `GITHUB_TOKEN=... gittop` away.
//
// `config_path` is only used to say where a config-file token came from.
Token ResolveToken(const model::RemoteRef& ref, const config::Config& config,
                   const std::string& config_path);

}  // namespace gittop::remote
