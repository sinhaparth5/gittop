#pragma once

#include <atomic>

#include "model/remote.hpp"
#include "remote/http.hpp"
#include "remote/token.hpp"

namespace gittop::remote {

// Blocking. Asks the provider for the repository behind `ref` and normalizes
// the answer into model::RepoInfo, which is the same shape whichever API
// replied. Every failure path comes back as a snapshot in FetchState::Failed
// with an error worth reading, never as an exception.
//
// This is the only place that knows GitHub says `stargazers_count` and GitLab
// says `star_count`. If a UI file ever needs that fact, the seam has leaked.
model::RemoteSnapshot FetchRepoInfo(const model::RemoteRef& ref, const Token& token,
                                    HttpClient& client,
                                    const std::atomic<bool>* cancel = nullptr);

}  // namespace gittop::remote
