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

FetchContent_MakeAvailable(ftxui libgit2)

# ---------------------------------------------------------------- gittop
add_executable(gittop
  src/main.cpp
  src/app.cpp
  src/git/graph.cpp
  src/git/repository.cpp
  src/ui/graph_panel.cpp
  src/ui/history_panel.cpp
  src/ui/panels.cpp
  src/ui/theme.cpp
  src/ui/widgets.cpp
)

# libgit2package only exports its include path for installed consumers, so an
# in-tree build has to point at the source headers itself.
target_include_directories(gittop PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${libgit2_SOURCE_DIR}/include
)

target_link_libraries(gittop PRIVATE
  ftxui::component
  ftxui::dom
  ftxui::screen
  libgit2package
)

target_compile_options(gittop PRIVATE -Wall -Wextra -Wpedantic)
