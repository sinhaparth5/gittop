#pragma once

#include <string>

// Shared between the git/ translation units and nothing else. Repository's
// methods are split across several files — reading a diff and driving a rebase
// have nothing to say to each other and do not belong in one 1500-line file —
// and this is the small amount they all need.

struct git_repository;

namespace gittop::git {

class Repository;

// libgit2's thread-local last error, or a stand-in when it has none. Every
// failure path in git/ ends here, so a message the user sees is libgit2's own
// words rather than a code translated twice.
std::string LastError();

// Why a command that rewrites the working tree cannot run now, or null when it
// can: an operation in progress, or uncommitted changes it would take with it.
// Refused rather than stashed, as a rebase is. In git/pick.cpp.
const char* Blocked(Repository& self, git_repository* repo);

}  // namespace gittop::git
