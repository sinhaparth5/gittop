#include "remote/provider.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

namespace gittop::remote {
namespace {

using model::Provider;
using model::RemoteRef;

std::string Lower(std::string in) {
  std::transform(in.begin(), in.end(), in.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return in;
}

bool EndsWith(const std::string& subject, const std::string& suffix) {
  return subject.size() >= suffix.size() &&
         subject.compare(subject.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool Contains(const std::string& subject, const std::string& needle) {
  return subject.find(needle) != std::string::npos;
}

// Everything after the last '@' is the host; anything before it is credentials
// we deliberately forget. Also strips a :port, which no API base wants.
std::string HostFromAuthority(const std::string& authority) {
  const std::size_t at = authority.rfind('@');
  std::string host = at == std::string::npos ? authority : authority.substr(at + 1);

  if (!host.empty() && host.front() == '[') {  // bracketed IPv6
    const std::size_t close = host.find(']');
    if (close != std::string::npos) {
      return host.substr(0, close + 1);
    }
    return host;
  }

  const std::size_t colon = host.find(':');
  if (colon != std::string::npos) {
    return host.substr(0, colon);
  }
  return host;
}

void SplitPath(const std::string& raw_path, RemoteRef* ref) {
  std::string path = raw_path;

  while (!path.empty() && path.front() == '/') {
    path.erase(path.begin());
  }
  while (!path.empty() && path.back() == '/') {
    path.pop_back();
  }
  if (EndsWith(path, ".git")) {
    path.resize(path.size() - 4);
  }
  if (path.empty()) {
    return;
  }

  const std::size_t slash = path.rfind('/');
  if (slash == std::string::npos) {
    // A single segment is a bare project with no namespace. Neither provider
    // has an API for that, so leaving owner empty keeps `valid()` false.
    ref->repo = path;
    return;
  }
  ref->owner = path.substr(0, slash);
  ref->repo = path.substr(slash + 1);
}

// The guess: exact cloud hostnames first, then a substring check that catches
// the overwhelmingly common self-hosted naming (git.example.com is the case it
// misses, which is what the config override is for).
Provider GuessProvider(const std::string& host, bool* self_hosted) {
  const std::string lower = Lower(host);

  if (lower == "github.com" || lower == "www.github.com" || lower == "ssh.github.com") {
    *self_hosted = false;
    return Provider::GitHub;
  }
  if (lower == "gitlab.com" || lower == "www.gitlab.com" || lower == "altssh.gitlab.com") {
    *self_hosted = false;
    return Provider::GitLab;
  }
  if (Contains(lower, "github")) {
    *self_hosted = true;
    return Provider::GitHub;
  }
  if (Contains(lower, "gitlab")) {
    *self_hosted = true;
    return Provider::GitLab;
  }
  *self_hosted = false;
  return Provider::Unknown;
}

void FillEndpoints(RemoteRef* ref) {
  switch (ref->provider) {
    case Provider::GitHub:
      // GitHub Enterprise Server hangs its v3 API off the instance itself;
      // github.com serves it from a separate hostname.
      ref->api_base = ref->self_hosted ? "https://" + ref->host + "/api/v3"
                                       : "https://api.github.com";
      break;
    case Provider::GitLab:
      ref->api_base = "https://" + ref->host + "/api/v4";
      break;
    case Provider::Unknown:
      ref->api_base.clear();
      break;
  }

  if (ref->provider != Provider::Unknown && !ref->owner.empty() && !ref->repo.empty()) {
    ref->web_url = "https://" + ref->host + "/" + ref->owner + "/" + ref->repo;
  }
}

}  // namespace

model::RemoteRef ParseRemote(const std::string& name, const std::string& url) {
  RemoteRef ref;
  ref.name = name;
  ref.url = url;

  const std::size_t scheme_end = url.find("://");
  std::string authority;
  std::string path;

  if (scheme_end != std::string::npos) {
    const std::string scheme = Lower(url.substr(0, scheme_end));
    if (scheme == "file") {
      return ref;  // a local clone has no provider, and that is a fine answer
    }
    const std::string rest = url.substr(scheme_end + 3);
    const std::size_t slash = rest.find('/');
    authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    path = slash == std::string::npos ? std::string{} : rest.substr(slash);
  } else {
    // scp-style: user@host:path. The colon has to come before any slash,
    // otherwise this is a plain filesystem path with a colon in it.
    const std::size_t colon = url.find(':');
    const std::size_t slash = url.find('/');
    if (colon == std::string::npos || (slash != std::string::npos && slash < colon)) {
      return ref;  // local path
    }
    authority = url.substr(0, colon);
    path = url.substr(colon + 1);
  }

  ref.host = HostFromAuthority(authority);
  if (ref.host.empty()) {
    return ref;
  }
  SplitPath(path, &ref);

  ref.provider = GuessProvider(ref.host, &ref.self_hosted);
  FillEndpoints(&ref);
  return ref;
}

void ApplyHostOverrides(model::RemoteRef& ref, const config::Config& config) {
  if (ref.host.empty()) {
    return;
  }

  const std::string provider = Lower(config.HostValue(ref.host, "provider"));
  if (provider == "github") {
    ref.provider = Provider::GitHub;
    ref.self_hosted = Lower(ref.host) != "github.com";
    FillEndpoints(&ref);
  } else if (provider == "gitlab") {
    ref.provider = Provider::GitLab;
    ref.self_hosted = Lower(ref.host) != "gitlab.com";
    FillEndpoints(&ref);
  }

  const std::string api = config.HostValue(ref.host, "api");
  if (!api.empty()) {
    ref.api_base = api;
    while (!ref.api_base.empty() && ref.api_base.back() == '/') {
      ref.api_base.pop_back();
    }
  }
}

model::RemoteRef ChooseRemote(const std::vector<model::RemoteRef>& remotes) {
  if (remotes.empty()) {
    return {};
  }
  for (const RemoteRef& ref : remotes) {
    if (ref.name == "origin") {
      return ref;
    }
  }
  for (const RemoteRef& ref : remotes) {
    if (ref.valid()) {
      return ref;
    }
  }
  return remotes.front();
}

}  // namespace gittop::remote
