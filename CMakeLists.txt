cmake_minimum_required(VERSION 3.24)
project(gittop VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(FetchContent)

# ----------------------------------------------------------------- FTXUI
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
  GIT_TAG        v7.0.3
  GIT_SHALLOW    TRUE)

# ---------------------------------------------------------------- libgit2
# Phase 1 only reads and writes the local repository, so the HTTPS and SSH
# transports stay off: they drag in OpenSSL and libssh2 for capability nothing
# uses yet. Phase 5 (push/pull) is where both get turned back on, and the SSH
# side is the one to budget time for.
set(BUILD_SHARED_LIBS OFF          CACHE BOOL   "" FORCE)
set(BUILD_TESTS       OFF          CACHE BOOL   "" FORCE)
set(BUILD_CLI         OFF          CACHE BOOL   "" FORCE)
set(BUILD_EXAMPLES    OFF          CACHE BOOL   "" FORCE)
set(BUILD_FUZZERS     OFF          CACHE BOOL   "" FORCE)
set(USE_SSH           OFF          CACHE STRING "" FORCE)
set(USE_HTTPS         OFF          CACHE STRING "" FORCE)
set(USE_GSSAPI        OFF          CACHE BOOL   "" FORCE)
set(USE_NTLMCLIENT    OFF          CACHE BOOL   "" FORCE)
set(REGEX_BACKEND     "builtin"    CACHE STRING "" FORCE)

FetchContent_Declare(libgit2
  GIT_REPOSITORY https://github.com/libgit2/libgit2.git
  GIT_TAG        v1.9.6
  GIT_SHALLOW    TRUE)

# ---------------------------------------------------------- nlohmann/json
# The release tarball rather than the git repository: it is a couple of hundred
# kilobytes against a clone that is three orders of magnitude larger, and the
# test suite is the bulk of what the clone would bring.
set(JSON_BuildTests     OFF CACHE BOOL "" FORCE)
set(JSON_Install        OFF CACHE BOOL "" FORCE)
set(JSON_MultipleHeaders OFF CACHE BOOL "" FORCE)

FetchContent_Declare(nlohmann_json
  URL      https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)

FetchContent_MakeAvailable(ftxui libgit2 nlohmann_json)

# ------------------------------------------------------------------ curl
# Taken from the system rather than vendored. libcurl is one of the very few
# libraries where the distribution's build is the one you want: it carries the
# platform's CA bundle and its TLS backend, and a self-built copy would have to
# be told where the system's certificates live before it could verify anything.
find_package(CURL 7.68 REQUIRED)

# ---------------------------------------------------------------- gittop
add_executable(gittop
  src/main.cpp
  src/app.cpp
  src/config/config.cpp
  src/git/graph.cpp
  src/git/repository.cpp
  src/remote/api.cpp
  src/remote/client.cpp
  src/remote/http.cpp
  src/remote/pipelines.cpp
  src/remote/provider.cpp
  src/remote/token.cpp
  src/ui/graph_panel.cpp
  src/ui/history_panel.cpp
  src/ui/panels.cpp
  src/ui/pipeline_panel.cpp
  src/ui/remote_panel.cpp
  src/ui/theme.cpp
  src/ui/widgets.cpp
)

# libgit2package only exports its include path for installed consumers, so an
# in-tree build has to point at the source headers itself.
target_include_directories(gittop PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${libgit2_SOURCE_DIR}/include
)

find_package(Threads REQUIRED)

target_link_libraries(gittop PRIVATE
  ftxui::component
  ftxui::dom
  ftxui::screen
  libgit2package
  CURL::libcurl
  nlohmann_json::nlohmann_json
  Threads::Threads
)

target_compile_options(gittop PRIVATE -Wall -Wextra -Wpedantic)
