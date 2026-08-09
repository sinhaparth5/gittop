#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "app.hpp"
#include "config/config.hpp"
#include "git/repository.hpp"
#include "remote/http.hpp"

namespace {

void PrintUsage() {
  std::cout << "gittop — a terminal dashboard for Git and CI\n"
               "\n"
               "usage: gittop [options] [path]\n"
               "\n"
               "  path              repository to open; defaults to the current directory\n"
               "\n"
               "  --config <file>   read this config instead of the default\n"
               "  --config-path     print where the config is read from, and exit\n"
               "  --init-config     write a commented starter config, and exit\n"
               "  --version         print the version, and exit\n"
               "  -h, --help        this text\n"
               "\n"
               "Tokens are read from the environment first (GITTOP_TOKEN, then\n"
               "GITHUB_TOKEN / GH_TOKEN or GITLAB_TOKEN / CI_JOB_TOKEN), then from\n"
               "the config file. Without one, public repositories still work.\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string start_path;
  std::string config_path;
  bool init_config = false;
  bool show_config_path = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      PrintUsage();
      return 0;
    }
    if (arg == "--version") {
      std::cout << "gittop 0.1.0\n";
      return 0;
    }
    if (arg == "--init-config") {
      init_config = true;
      continue;
    }
    if (arg == "--config-path") {
      show_config_path = true;
      continue;
    }
    if (arg == "--config") {
      if (i + 1 >= argc) {
        std::cerr << "gittop: --config needs a path\n";
        return 1;
      }
      config_path = argv[++i];
      continue;
    }
    if (!arg.empty() && arg.front() == '-') {
      std::cerr << "gittop: unknown option " << arg << "\n";
      return 1;
    }
    start_path = arg;
  }

  if (config_path.empty()) {
    config_path = gittop::config::Config::DefaultPath();
  }

  if (show_config_path) {
    std::cout << config_path << '\n';
    return 0;
  }

  if (init_config) {
    std::string error;
    if (!gittop::config::Config::WriteTemplate(config_path, &error)) {
      std::cerr << "gittop: " << error << '\n';
      return 1;
    }
    std::cout << "wrote " << config_path << " (0600)\n";
    return 0;
  }

  // Both libraries are process-wide and neither init is thread-safe, so they
  // are constructed here, before anything can spawn a worker.
  const gittop::git::Library libgit2;
  const gittop::remote::HttpLibrary http;
  if (!http.ok()) {
    std::cerr << "gittop: could not initialise HTTP support\n";
    return 1;
  }

  if (start_path.empty()) {
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

  // A config that does not parse is worth saying out loud, but not worth
  // refusing to start over: everything except the remote view works without it.
  std::string config_error;
  gittop::config::Config config = gittop::config::Config::Load(config_path, &config_error);
  if (!config_error.empty()) {
    std::cerr << "gittop: " << config_error << '\n';
  }

  gittop::App app(std::move(*repo), std::move(config), config_path);
  return app.Run();
}
