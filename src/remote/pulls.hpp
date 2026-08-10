#pragma once

#include <atomic>
#include <string>

#include "model/pull.hpp"
#include "model/remote.hpp"
#include "remote/http.hpp"
#include "remote/token.hpp"

namespace gittop::remote {

// Blocking. Lists the open pull requests or merge requests and normalizes both
// into model::PullRequest.
//
// `head_branch` is only used to mark the row for the branch you are standing
// on; it does not filter, because the point of the view is the ones you are not
// on. Empty is fine and marks nothing.
//
// This file is the only place that knows GitHub says `head.ref` where GitLab
// says `source_branch`, that GitLab has a fourth state gittop folds away, and
// that only one of the two reports whether a branch would merge cleanly.
model::PullSnapshot FetchPulls(const model::RemoteRef& ref, const Token& token,
                               const std::string& head_branch, int limit, HttpClient& client,
                               const std::atomic<bool>* cancel = nullptr);

}  // namespace gittop::remote
