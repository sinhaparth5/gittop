#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "model/remote.hpp"
#include "remote/http.hpp"
#include "remote/token.hpp"

// The parts of talking to a provider that are the same whatever is being asked
// for: how a URL is escaped, how a request is authenticated, how a rate limit
// is read back, and how a status code becomes a sentence.
//
// This lives apart from client.cpp because Phase 4 added a second endpoint
// family. Everything here was private to client.cpp first and moved out the
// moment a second caller needed it, which is the only reason it is shared.
namespace gittop::remote {

std::int64_t NowSeconds();

// Percent-encodes everything a path segment can contain, slashes included.
// GitLab addresses a project by its namespaced path with the slashes encoded,
// which is the one place a URL is assembled from user data.
std::string PercentEncode(const std::string& in);

// Both APIs hand back RFC 3339. timegm rather than mktime: these are UTC, and
// reading them as local time would shift every timestamp by the offset.
std::int64_t ParseIso8601(const std::string& text);

// Absent, null and wrong-typed all come back as the empty/negative answer. The
// alternative is a exception from operator[] on a payload that changed shape,
// which is not worth crashing a dashboard over.
std::string StringField(const nlohmann::json& object, const char* key);
int IntField(const nlohmann::json& object, const char* key);
bool BoolField(const nlohmann::json& object, const char* key);

// Reads a nested string, e.g. Nested(run, "actor", "login"). Returns empty when
// any hop is missing rather than when only the last one is.
std::string NestedString(const nlohmann::json& object, const char* key, const char* sub);

// GitHub sends x-ratelimit-*; GitLab sends RateLimit-* on instances that
// enforce one and nothing at all on those that do not. Absent is not zero.
model::RateLimit ReadRateLimit(const HttpResponse& response, model::Provider provider);

// Authorization: Bearer on GitHub, PRIVATE-TOKEN on GitLab. Getting this wrong
// fails as a 401 that looks exactly like a bad token, so it is written once.
std::vector<std::string> HeadersFor(const model::RemoteRef& ref, const Token& token);

// The project's own endpoint: /repos/{owner}/{repo} or /projects/{encoded}.
// Every other endpoint in this codebase is a suffix on this one.
std::string ProjectEndpoint(const model::RemoteRef& ref);

// Turns a status code into something a person can act on. `subject` names what
// was being asked for ("repository", "workflow runs") so a 404 reads as the
// thing that was missing rather than as a generic failure.
//
// The distinction that matters most is 404 while anonymous, which nearly always
// means private rather than missing.
void DescribeStatus(long status, const Token& token, const model::RateLimit& rate,
                    model::Provider provider, const char* subject, std::string* error,
                    std::string* hint);

}  // namespace gittop::remote
