#pragma once

#include <string>

// Shared between the git/ translation units and nothing else. Repository's
// methods are split across several files — reading a diff and driving a rebase
// have nothing to say to each other and do not belong in one 1500-line file —
// and this is the small amount they all need.

namespace gittop::git {

// libgit2's thread-local last error, or a stand-in when it has none. Every
// failure path in git/ ends here, so a message the user sees is libgit2's own
// words rather than a code translated twice.
std::string LastError();

}  // namespace gittop::git
