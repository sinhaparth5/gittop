#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "config/config.hpp"
#include "model/remote.hpp"
#include "remote/http.hpp"

// Signing in, by the one OAuth flow that suits a program with no browser and no
// redirect URI to hand: RFC 8628's device authorization grant. gittop asks the
// provider for a code, shows the user a URL and eight characters to type into
// it, and polls until somebody approves.
//
// The alternative — an authorization-code flow — needs a loopback HTTP server
// listening for the redirect, which is a port, a firewall prompt and a race
// with whatever else is on that port, all so a terminal program can do what the
// device flow does with two POSTs. It is also the only flow that works over
// ssh, which is a normal way to reach a machine you want a dashboard on.
//
// Provider differences are folded here and nowhere else, in keeping with the
// rule that model/ and ui/ never learn who replied.
namespace gittop::remote {

// A device flow needs a registered OAuth application, and an application is
// per-instance: a client_id issued by github.com means nothing to a GitHub
// Enterprise Server, and gittop cannot register one on a host it has never
// heard of. So this can legitimately answer "no", and the sign-in falls back to
// the guided personal access token instead of failing.
//
// Looked up as hosts."<host>".client_id, falling back to a built-in id for the
// public instances if one has been compiled in.
std::string ClientIdFor(const model::RemoteRef& ref, const config::Config& config);

// Where the host serves its OAuth endpoints and its web UI, which is not where
// it serves its API: github.com issues device codes while api.github.com
// answers everything else in this codebase, so this cannot be derived from
// ref.api_base.
//
// Defaults to https://<host> and is overridable as hosts."<host>".oauth. The
// override is not decoration: RemoteRef::host has its port stripped — no API
// base wants one — so a self-hosted instance reached on a non-standard port
// would otherwise have its device request sent to port 443 of the same name.
std::string OAuthOrigin(const model::RemoteRef& ref, const config::Config& config);

// What gittop asks for, and why it is not simply "everything":
//
//   GitHub  repo, read:org
//   GitLab  read_api, write_repository
//
// `repo` is broader than a dashboard needs and there is no read-only classic
// scope that covers private repositories, so this is the floor rather than a
// choice. Both sets include write to the repository because the token is also
// what authenticates an https push — see UsernameFor in app.cpp. A read-only
// token would leave `P` failing with a 403 that looks like a gittop bug.
std::string ScopesFor(model::Provider provider);

// The provider's own token-creation page, with the name and the scopes above
// already filled in, so the fallback is still one click and a paste rather than
// a hunt through a settings tree. Empty when the provider is unknown.
std::string TokenPageUrl(const model::RemoteRef& ref, const std::string& origin);

// What comes back from the first POST. `device_code` is a secret for the length
// of the flow — it is what a poller presents to claim the resulting token — so
// it is kept out of anything rendered, exactly like a token.
struct DeviceCode {
  std::string device_code;
  std::string user_code;         // shown: the eight characters to type
  std::string verification_uri;  // shown: where to type them
  std::string verification_uri_complete;  // both in one; GitLab sends it, GitHub does not

  int interval_seconds = 5;
  std::int64_t expires_at = 0;  // unix seconds, 0 when the provider did not say

  std::string error;  // human-readable, safe to print
  std::string hint;

  bool ok() const { return !device_code.empty() && !user_code.empty(); }
};

// Where a poll got to. Pending and SlowDown both mean "ask again"; the second
// also means the interval was too short and has been widened.
enum class PollState {
  Pending,
  SlowDown,
  Granted,
  Denied,
  Expired,
  Failed,
};

struct PollResult {
  PollState state = PollState::Failed;
  std::string token;  // set only when state is Granted, and never rendered
  int interval_seconds = 0;  // > 0 when the server asked for a longer wait
  std::string error;
  std::string hint;
};

// Both blocking, both meant for a worker thread, both polling `cancel` so that
// quitting mid-flow does not wait out a timeout.
DeviceCode BeginDeviceFlow(const model::RemoteRef& ref, const std::string& origin,
                           const std::string& client_id, HttpClient& http,
                           const std::atomic<bool>* cancel);

PollResult PollDeviceFlow(const model::RemoteRef& ref, const std::string& origin,
                          const std::string& client_id, const std::string& device_code,
                          HttpClient& http, const std::atomic<bool>* cancel);

// Hands a URL to the desktop's browser, without a shell anywhere in the path:
// the URL comes from a server response, and a server response reaching
// system(3) is a command injection with extra steps. Refuses anything that is
// not a plain https URL for the same reason.
//
// Returns false when there is no opener or the URL is not one to open, which is
// not an error worth interrupting a sign-in for — the URL is on screen anyway.
bool OpenInBrowser(const std::string& url);

}  // namespace gittop::remote
