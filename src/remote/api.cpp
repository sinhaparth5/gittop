#include "remote/api.hpp"

#include <ctime>
#include <exception>
#include <iomanip>
#include <sstream>

namespace gittop::remote {
namespace {

using model::Provider;
using model::RateLimit;

int HeaderInt(const HttpResponse& response, const char* name) {
  const std::string value = response.Header(name);
  if (value.empty()) {
    return -1;
  }
  try {
    return std::stoi(value);
  } catch (const std::exception&) {
    return -1;
  }
}

}  // namespace

std::int64_t NowSeconds() {
  return static_cast<std::int64_t>(std::time(nullptr));
}

std::string PercentEncode(const std::string& in) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(in.size() * 3);
  for (const unsigned char c : in) {
    const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                            c == '~';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

std::int64_t ParseIso8601(const std::string& text) {
  if (text.empty()) {
    return 0;
  }
  std::tm tm{};
  std::istringstream stream(text);
  stream >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
  if (stream.fail()) {
    return 0;
  }
  return static_cast<std::int64_t>(timegm(&tm));
}

std::string StringField(const nlohmann::json& object, const char* key) {
  if (!object.is_object() || !object.contains(key) || !object[key].is_string()) {
    return {};
  }
  return object[key].get<std::string>();
}

int IntField(const nlohmann::json& object, const char* key) {
  if (!object.is_object() || !object.contains(key) || !object[key].is_number_integer()) {
    return -1;
  }
  return object[key].get<int>();
}

bool BoolField(const nlohmann::json& object, const char* key) {
  return object.is_object() && object.contains(key) && object[key].is_boolean() &&
         object[key].get<bool>();
}

std::string NestedString(const nlohmann::json& object, const char* key, const char* sub) {
  if (!object.is_object() || !object.contains(key) || !object[key].is_object()) {
    return {};
  }
  return StringField(object[key], sub);
}

RateLimit ReadRateLimit(const HttpResponse& response, Provider provider) {
  RateLimit rate;
  const char* limit_name = provider == Provider::GitHub ? "x-ratelimit-limit" : "ratelimit-limit";
  const char* remaining_name =
      provider == Provider::GitHub ? "x-ratelimit-remaining" : "ratelimit-remaining";
  const char* reset_name = provider == Provider::GitHub ? "x-ratelimit-reset" : "ratelimit-reset";

  rate.limit = HeaderInt(response, limit_name);
  rate.remaining = HeaderInt(response, remaining_name);
  const int reset = HeaderInt(response, reset_name);
  if (reset > 0) {
    rate.reset = reset;
  }
  rate.known = rate.limit > 0 && rate.remaining >= 0;
  return rate;
}

std::vector<std::string> HeadersFor(const model::RemoteRef& ref, const Token& token) {
  std::vector<std::string> headers;
  switch (ref.provider) {
    case Provider::GitHub:
      headers.emplace_back("Accept: application/vnd.github+json");
      headers.emplace_back("X-GitHub-Api-Version: 2022-11-28");
      if (token.present()) {
        headers.emplace_back("Authorization: Bearer " + token.value);
      }
      break;
    case Provider::GitLab:
      headers.emplace_back("Accept: application/json");
      if (token.present()) {
        headers.emplace_back("PRIVATE-TOKEN: " + token.value);
      }
      break;
    case Provider::Unknown:
      break;
  }
  return headers;
}

std::string ProjectEndpoint(const model::RemoteRef& ref) {
  switch (ref.provider) {
    case Provider::GitHub:
      return ref.api_base + "/repos/" + PercentEncode(ref.owner) + "/" + PercentEncode(ref.repo);
    case Provider::GitLab:
      // One encoded path segment, subgroups and all.
      return ref.api_base + "/projects/" + PercentEncode(ref.owner + "/" + ref.repo);
    case Provider::Unknown:
      break;
  }
  return {};
}

void DescribeStatus(long status, const Token& token, const RateLimit& rate, Provider provider,
                    const char* subject, std::string* error, std::string* hint) {
  switch (status) {
    case 401:
      *error = "the token was rejected";
      *hint = token.source == model::TokenSource::Environment
                  ? "check " + token.origin + ", or unset it to browse anonymously"
                  : "check the token in your config file";
      return;
    case 403:
      if (rate.known && rate.remaining == 0) {
        *error = "API rate limit reached";
        *hint = token.present() ? "the limit resets shortly"
                                : "an authenticated token raises the limit substantially";
        return;
      }
      *error = "access forbidden";
      *hint = token.present() ? std::string("the token may lack the scope to read ") + subject
                              : std::string("reading ") + subject + " here needs a token";
      return;
    case 404:
      *error = std::string(subject) + " not found";
      *hint = token.present() ? "check the remote URL, and that the token can see it"
                              : "if it is private, a token is needed to read it";
      return;
    case 429:
      *error = "rate limited";
      *hint = "too many requests; try again in a moment";
      return;
    default:
      break;
  }

  if (status >= 500) {
    *error = std::string(provider == Provider::GitHub ? "GitHub" : "GitLab") + " returned " +
             std::to_string(status);
    *hint = "the API is having trouble; `r` retries";
    return;
  }
  *error = "unexpected response " + std::to_string(status);
  *hint = "`r` retries";
}

}  // namespace gittop::remote
