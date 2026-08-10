#pragma once

#include <atomic>
#include <string>

#include "model/pipeline.hpp"
#include "model/remote.hpp"
#include "remote/http.hpp"
#include "remote/token.hpp"

namespace gittop::remote {

// Blocking. Lists the CI runs for `branch` and normalizes them into
// model::Pipeline, which is the same shape whichever API replied.
//
// An empty `branch` asks for every branch, which is what a detached HEAD gets:
// there is nothing to filter by, and returning nothing would be worse than
// returning the repository's recent runs.
//
// This file and FetchJobs below are the only places that know GitHub splits a
// run's state across `status` and `conclusion` while GitLab answers with one
// field. If a file under ui/ ever needs that fact, the seam has leaked.
model::PipelineSnapshot FetchPipelines(const model::RemoteRef& ref, const Token& token,
                                       const std::string& branch, int limit, HttpClient& client,
                                       const std::atomic<bool>* cancel = nullptr);

// Blocking. The jobs of one run, fetched only when someone asks to see them:
// doing it with the list would cost one request per row and spend a rate limit
// on data nobody opened.
model::JobList FetchJobs(const model::RemoteRef& ref, const Token& token,
                         const std::string& pipeline_id, HttpClient& client,
                         const std::atomic<bool>* cancel = nullptr);

}  // namespace gittop::remote
