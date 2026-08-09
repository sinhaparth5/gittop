#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

#include "app.hpp"
#include "git/repository.hpp"

int main(int argc, char** argv) {
  const gittop::git::Library libgit2;

  std::string start_path;
  if (argc > 1) {
    start_path = argv[1];
  } else {
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (ec) {
      std::cerr << "gittop: cannot read the current directory: " << ec.message() << '\n';
      return 1;
    }
    start_path = cwd.string();
  }

  std::string error;
  std::optional<gittop::git::Repository> repo =
      gittop::git::Repository::Discover(start_path, &error);
  if (!repo) {
    std::cerr << "gittop: " << error << '\n';
    return 1;
  }

  gittop::App app(std::move(*repo));
  return app.Run();
}
