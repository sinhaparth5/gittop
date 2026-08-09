#pragma once

#include <string>
#include <vector>

#include "config/config.hpp"
#include "model/remote.hpp"

namespace gittop::remote {

// Turns a configured remote URL into a RemoteRef. Understands the three shapes
// git actually hands out: scheme URLs, scp-style host:path, and local paths
// (which come back as Unknown, correctly).
//
// Credentials embedded in a URL are dropped from every derived field, so a
// remote configured as https://user:token@host/... never reaches the screen
// with the token still in it.
model::RemoteRef ParseRemote(const std::string& name, const std::string& url);

// Lets a config file name the provider and API base for a host gittop cannot
// guess. Applied after parsing so the guess stays the default.
void ApplyHostOverrides(model::RemoteRef& ref, const config::Config& config);

// Picks the one to show: `origin` when it exists, otherwise the first that
// parsed into something usable, otherwise the first at all. Returns an empty
// ref when the list is empty.
model::RemoteRef ChooseRemote(const std::vector<model::RemoteRef>& remotes);

}  // namespace gittop::remote
