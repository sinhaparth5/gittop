#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "git/repository.hpp"

// libgit2's init is process-wide and not thread-safe, so it happens once here,
// before any test can touch a repository — the same rule main() keeps.
int main(int argc, char** argv) {
  gittop::git::Library git;
  doctest::Context context(argc, argv);
  return context.run();
}
