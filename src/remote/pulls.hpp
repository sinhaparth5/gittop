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

// Blocking. Opens one, and is the only write anywhere under remote/.
//
// The four fields of the draft go out under whichever names this provider uses
// and the reply comes back through the same normalization the list uses, so
// nothing above this line learns that GitHub wants `head` where GitLab wants
// `source_branch`.
//
// This one does *not* retry. Every other request in this codebase is a read and
// HttpClient's retry loop is safe on it; a create is not idempotent, and a
// transport failure after the server has already made the pull request would
// make a second one on the way back. Being told the request failed when it
// worked is recoverable — the list refreshes and it is there. Two open pull
// requests for one branch is not.
model::PullCreated CreatePull(const model::RemoteRef& ref, const Token& token,
                              const model::PullDraft& draft, HttpClient& client,
                              const std::atomic<bool>* cancel = nullptr);

}  // namespace gittop::remote
