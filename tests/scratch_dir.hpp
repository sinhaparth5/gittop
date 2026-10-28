#pragma once

#include <filesystem>
#include <random>
#include <string>
#include <system_error>

// A fresh directory under the system temp dir, removed when it goes out of
// scope. Fixtures are built in here at test time rather than checked in, so they
// are readable as code and cannot rot into a blob nobody can regenerate.
class ScratchDir {
 public:
  ScratchDir() {
    std::random_device random;
    path_ = std::filesystem::temp_directory_path() /
            ("gittop-test-" + std::to_string(random()) + std::to_string(random()));
    std::filesystem::create_directories(path_);
  }
  ~ScratchDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  ScratchDir(const ScratchDir&) = delete;
  ScratchDir& operator=(const ScratchDir&) = delete;

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};
