#include "remote/oauth.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <string>
#include <vector>

#include "platform/platform.hpp"
#include "remote/api.hpp"

namespace gittop::remote {
namespace {

using model::Provider;

// No OAuth application has been registered for gittop, so there is no id to
// compile in and every host falls back to the guided personal access token.
// Filling these in is a packaging decision rather than a code one: an id is
// public by design in a device flow — there is no client secret, which is the
// whole reason the flow is safe to run in a distributed binary — but it ties
// the build to one registered application, and a GPL source tree that anyone
// can rebuild should not have that baked in without the maintainer choosing it.
//
// hosts."<host>".client_id overrides both, and is the only way a self-hosted
// instance can ever have one.
constexpr const char* kGitHubClientId = "";
constexpr const char* kGitLabClientId = "";

// RFC 8628. Both providers spell it exactly this way and reject anything else.
constexpr const char* kDeviceGrantType = "urn:ietf:params:oauth:grant-type:device_code";

std::string DeviceEndpoint(const model::RemoteRef& ref, const std::string& origin) {
  if (origin.empty()) {
    return {};
  }
  switch (ref.provider) {
    case Provider::GitHub:
      return origin + "/login/device/code";
    case Provider::GitLab:
      return origin + "/oauth/authorize_device";
    case Provider::Unknown:
      break;
  }
  return {};
}

std::string TokenEndpoint(const model::RemoteRef& ref, const std::string& origin) {
  if (origin.empty()) {
    return {};
  }
  switch (ref.provider) {
    case Provider::GitHub:
      return origin + "/login/oauth/access_token";
    case Provider::GitLab:
      return origin + "/oauth/token";
    case Provider::Unknown:
      break;
  }
  return {};
}

// Both endpoints answer JSON only when asked to. GitHub's default is form
// encoding, and a form-encoded body parsed as JSON is a failure that looks
// exactly like a rejected sign-in.
std::vector<std::string> FormHeaders() {
  return {
      "Accept: application/json",
      "Content-Type: application/x-www-form-urlencoded",
  };
}

std::string Field(const std::string& key, const std::string& value) {
  return PercentEncode(key) + "=" + PercentEncode(value);
}

// Absent, null or wrong-typed reads as absent, the same contract the rest of
// remote/ already parses under.
nlohmann::json ParseBody(const std::string& body) {
  return nlohmann::json::parse(body, nullptr, false);
}

// A 404 on the device endpoint is the single most likely failure on a
// self-hosted instance and it does not mean what it says: GitLab only grew the
// device grant in 17.2, and GitHub Enterprise Server serves the endpoint but
// 404s it when the application has device flow switched off.
std::string DeviceHintFor(const model::RemoteRef& ref, long status) {
  if (status == 404) {
    return ref.provider == Provider::GitLab
               ? "this instance may predate GitLab 17.2, which added the device grant"
               : "the OAuth app may not have device flow enabled";
  }
  if (status == 401 || status == 422) {
    return "check hosts.\"" + ref.host + "\".client_id";
  }
  return {};
}

}  // namespace

std::string ClientIdFor(const model::RemoteRef& ref, const config::Config& config) {
  if (!ref.host.empty()) {
    const std::string configured = config.HostValue(ref.host, "client_id");
    if (!configured.empty()) {
      return configured;
    }
  }
  // A built-in id belongs to one instance and must never be offered to another,
  // self-hosted or not — sending github.com's id to an unrelated host leaks
  // nothing but guarantees a confusing failure.
  if (ref.host == "github.com") {
    return kGitHubClientId;
  }
  if (ref.host == "gitlab.com") {
    return kGitLabClientId;
  }
  return {};
}

std::string OAuthOrigin(const model::RemoteRef& ref, const config::Config& config) {
  if (ref.host.empty()) {
    return {};
  }
  std::string configured = config.HostValue(ref.host, "oauth");
  if (!configured.empty()) {
    while (!configured.empty() && configured.back() == '/') {
      configured.pop_back();
    }
    return configured;
  }
  return "https://" + ref.host;
}

std::string ScopesFor(Provider provider) {
  switch (provider) {
    case Provider::GitHub:
      return "repo read:org";
    case Provider::GitLab:
      return "read_api write_repository";
    case Provider::Unknown:
      break;
  }
  return {};
}

std::string TokenPageUrl(const model::RemoteRef& ref, const std::string& origin) {
  if (origin.empty()) {
    return {};
  }
  switch (ref.provider) {
    case Provider::GitHub:
      // Scopes are comma-separated here and space-separated in the OAuth
      // request. Same list, two spellings, both required.
      return origin + "/settings/tokens/new?description=gittop&scopes=repo,read:org";
    case Provider::GitLab:
      return origin +
             "/-/user_settings/personal_access_tokens"
             "?name=gittop&scopes=read_api,write_repository";
    case Provider::Unknown:
      break;
  }
  return {};
}

DeviceCode BeginDeviceFlow(const model::RemoteRef& ref, const std::string& origin,
                           const std::string& client_id, HttpClient& http,
                           const std::atomic<bool>* cancel) {
  DeviceCode out;

  const std::string endpoint = DeviceEndpoint(ref, origin);
  if (endpoint.empty() || client_id.empty()) {
    out.error = "no OAuth application is configured for " + ref.host;
    out.hint = "set hosts.\"" + ref.host + "\".client_id, or use a personal access token";
    return out;
  }

  HttpRequest request;
  request.url = endpoint;
  request.headers = FormHeaders();
  request.body = Field("client_id", client_id) + "&" + Field("scope", ScopesFor(ref.provider));
  request.max_attempts = 2;

  const HttpResponse response = http.Post(request, cancel);
  if (!response.transport_ok) {
    out.error = response.error.empty() ? "could not reach " + ref.host : response.error;
    return out;
  }

  const nlohmann::json body = ParseBody(response.body);
  if (!response.success() || body.is_discarded()) {
    const std::string described = StringField(body, "error_description");
    out.error = described.empty() ? "sign-in was refused (HTTP " + std::to_string(response.status) +
                                        ")"
                                  : described;
    out.hint = DeviceHintFor(ref, response.status);
    return out;
  }

  out.device_code = StringField(body, "device_code");
  out.user_code = StringField(body, "user_code");
  out.verification_uri = StringField(body, "verification_uri");
  out.verification_uri_complete = StringField(body, "verification_uri_complete");

  const int interval = IntField(body, "interval");
  if (interval > 0) {
    out.interval_seconds = interval;
  }
  const int expires_in = IntField(body, "expires_in");
  if (expires_in > 0) {
    out.expires_at = NowSeconds() + expires_in;
  }

  if (!out.ok()) {
    out.error = "the provider's reply did not contain a device code";
  }
  return out;
}

PollResult PollDeviceFlow(const model::RemoteRef& ref, const std::string& origin,
                          const std::string& client_id, const std::string& device_code,
                          HttpClient& http, const std::atomic<bool>* cancel) {
  PollResult out;

  const std::string endpoint = TokenEndpoint(ref, origin);
  if (endpoint.empty() || client_id.empty() || device_code.empty()) {
    out.error = "the sign-in was not started";
    return out;
  }

  HttpRequest request;
  request.url = endpoint;
  request.headers = FormHeaders();
  request.body = Field("client_id", client_id) + "&" + Field("device_code", device_code) + "&" +
                 Field("grant_type", kDeviceGrantType);
  request.max_attempts = 2;

  const HttpResponse response = http.Post(request, cancel);
  if (!response.transport_ok) {
    // A poll is going to happen again in a few seconds anyway, so one failed
    // round trip is not a reason to tear the flow down. Only a definite answer
    // from the server ends it.
    out.state = PollState::Pending;
    out.error = response.error;
    return out;
  }

  const nlohmann::json body = ParseBody(response.body);
  if (body.is_discarded()) {
    out.error = "the provider's reply could not be read (HTTP " +
                std::to_string(response.status) + ")";
    return out;
  }

  const std::string token = StringField(body, "access_token");
  if (!token.empty()) {
    out.state = PollState::Granted;
    out.token = token;
    return out;
  }

  // The status code is deliberately not what decides this. GitHub answers a
  // pending authorization with 200 and an `error` field; GitLab answers the
  // same state with 400 and the same field, which is what RFC 8628 actually
  // says. Reading the body rather than the code is the only thing that works
  // on both, and getting it from the code instead would make every GitLab
  // sign-in fail on the first poll.
  const std::string error = StringField(body, "error");
  const std::string described = StringField(body, "error_description");

  if (error == "authorization_pending") {
    out.state = PollState::Pending;
    return out;
  }
  if (error == "slow_down") {
    out.state = PollState::SlowDown;
    out.interval_seconds = IntField(body, "interval");
    return out;
  }
  if (error == "expired_token") {
    out.state = PollState::Expired;
    out.error = "the code expired before it was approved";
    return out;
  }
  if (error == "access_denied") {
    out.state = PollState::Denied;
    out.error = "the sign-in was declined";
    return out;
  }

  out.state = PollState::Failed;
  if (!described.empty()) {
    out.error = described;
  } else if (!error.empty()) {
    out.error = error;
  } else {
    out.error = "sign-in failed (HTTP " + std::to_string(response.status) + ")";
  }
  if (error == "unauthorized_client" || error == "invalid_client") {
    out.hint = "check hosts.\"" + ref.host + "\".client_id";
  }
  return out;
}

bool OpenInBrowser(const std::string& url) {
  // Deliberately strict, and this check is the whole reason handing the string
  // onwards is safe. It came off the network, and what happens to it next is a
  // command line: an exec here, and on Windows a substitution into the
  // registered protocol handler's own template, which is a command line being
  // built somewhere gittop cannot see. Anything that is not obviously a web URL
  // is not worth the argument about whether some opener would have survived it.
  if (url.rfind("https://", 0) != 0 || url.size() > 2048) {
    return false;
  }
  for (const unsigned char c : url) {
    // Control characters and space: a leading "-" would be read as a flag by an
    // opener, and whitespace would split one argument into two.
    if (c <= 0x20 || c == 0x7F) {
      return false;
    }
    // The shell and command-line metacharacters, none of which RFC 3986 allows
    // in a URI in the first place — so rejecting them costs nothing real and
    // closes the one hole the control-character check above leaves open. A
    // quote is the one that matters: it is 0x22, it passed the test above, and
    // it is exactly what would end an argument early inside a handler template
    // like `firefox.exe -url "%1"`.
    if (c == '"' || c == '\'' || c == '\\' || c == '`' || c == '<' || c == '>' ||
        c == '|' || c == '^' || c == '&' || c == '$') {
      return false;
    }
  }

  return platform::OpenUrl(url);
}

}  // namespace gittop::remote
