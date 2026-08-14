cmake_minimum_required(VERSION 3.24)

# CalVer, YYYY.MM.PATCH. The month keeps its leading zero — CMake preserves it
# through PROJECT_VERSION, so "2026.08.0" survives to the binary rather than
# arriving as "2026.8.0" and disagreeing with the tag it was built from.
#
# This is the only place the version is written. It reaches the program as
# GITTOP_VERSION below, so `--version`, the settings page and the splash cannot
# drift from each other or from the release; the workflow in
# .github/workflows/release.yml refuses a tag that does not match it.
project(gittop VERSION 2026.08.4 LANGUAGES C CXX)

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
# Phase 5 pushes and pulls, so the transports that were off through Phases 1-4
# come back on. Both backends are chosen to add no build-time dependency:
#
#   OpenSSL-Dynamic  dlopen()s libssl at run time instead of linking it, so the
#                    build needs no OpenSSL headers and the binary is not tied
#                    to the ABI of the 1.1 or 3.x it was compiled against. It
#                    still uses OpenSSL's own default certificate paths, which
#                    is the same reason libcurl is taken from the system below.
#
#   exec             runs the system `ssh` rather than reimplementing it with
#                    libssh2. That is not only the cheaper dependency, it is the
#                    better behaviour: gittop inherits ~/.ssh/config, the agent,
#                    the user's keys and their known_hosts, so a remote that
#                    works from the shell works here with no second setup.
set(BUILD_SHARED_LIBS OFF                CACHE BOOL   "" FORCE)
set(BUILD_TESTS       OFF                CACHE BOOL   "" FORCE)
set(BUILD_CLI         OFF                CACHE BOOL   "" FORCE)
set(BUILD_EXAMPLES    OFF                CACHE BOOL   "" FORCE)
set(BUILD_FUZZERS     OFF                CACHE BOOL   "" FORCE)
set(USE_SSH           "exec"             CACHE STRING "" FORCE)
set(USE_HTTPS         "OpenSSL-Dynamic"  CACHE STRING "" FORCE)
set(USE_GSSAPI        OFF                CACHE BOOL   "" FORCE)
set(USE_NTLMCLIENT    OFF                CACHE BOOL   "" FORCE)
set(REGEX_BACKEND     "builtin"          CACHE STRING "" FORCE)

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
  src/git/askpass.cpp
  src/git/diff.cpp
  src/git/graph.cpp
  src/git/rebase.cpp
  src/git/repository.cpp
  src/git/stash.cpp
  src/git/transfer.cpp
  src/remote/api.cpp
  src/remote/client.cpp
  src/remote/http.cpp
  src/remote/oauth.cpp
  src/remote/pipelines.cpp
  src/remote/provider.cpp
  src/remote/pulls.cpp
  src/remote/token.cpp
  src/ui/diff_panel.cpp
  src/ui/glyphs.cpp
  src/ui/graph_panel.cpp
  src/ui/history_panel.cpp
  src/ui/keymap.cpp
  src/ui/panels.cpp
  src/ui/pipeline_panel.cpp
  src/ui/pull_panel.cpp
  src/ui/remote_panel.cpp
  src/ui/settings_panel.cpp
  src/ui/signin_panel.cpp
  src/ui/stash_panel.cpp
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

target_compile_definitions(gittop PRIVATE GITTOP_VERSION="${PROJECT_VERSION}")

# ---------------------------------------------------------------- install
include(GNUInstallDirs)

install(TARGETS gittop RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT gittop)

# A TUI still earns a menu entry — btop ships one — and Terminal=true is the
# line that makes a launcher open a terminal for it rather than starting it
# headless, where it would draw its alternate screen into nothing.
install(FILES packaging/gittop.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
        COMPONENT gittop)

# The scalable icon is what current desktops prefer and the one that stays sharp
# at whatever size a panel asks for; the rasters below are for the environments
# that will not render SVG. Both live under hicolor, which is the theme every
# other theme falls back to, so the icon resolves regardless of what the user
# has selected.
install(FILES assets/logo.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
        RENAME gittop.svg
        COMPONENT gittop)

foreach(icon_size IN ITEMS 16 24 32 48 64 128 256)
  install(FILES assets/icons/gittop-${icon_size}.png
          DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${icon_size}x${icon_size}/apps
          RENAME gittop.png
          COMPONENT gittop)
endforeach()

install(FILES README.md CHANGELOG.md LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
        COMPONENT gittop)

# ---------------------------------------------------------------- package
# `cpack -G DEB` and `cpack -G TGZ` from the build directory. The release
# workflow runs both; neither needs anything installed that the build did not
# already need.
# Only gittop's own files. FetchContent brings libgit2's CMakeLists into this
# project, install rules and all, and libgit2 offers no option to turn them off
# — so without this the package ships /usr/include/git2/**, /usr/lib/libgit2.a
# and libgit2.pc, which is a 16 MB package that also collides file-for-file with
# libgit2-dev on any machine that has it. Every install() above is tagged
# COMPONENT gittop; libgit2's are untagged and therefore excluded here.
set(CPACK_COMPONENTS_ALL gittop)

# Component install has to be switched on per generator, or CPACK_COMPONENTS_ALL
# is quietly ignored and the monolithic install runs — which looks like it
# worked right up until you list the package contents. ALL_COMPONENTS_IN_ONE
# keeps it a single package and a single file name rather than one per
# component, which with one component would only be a longer name.
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)

# So the tarball unpacks into a directory of its own rather than emptying bin/
# and share/ into whatever the user was standing in. This is the component-install
# spelling of the setting — plain CPACK_INCLUDE_TOPLEVEL_DIRECTORY is ignored
# once CPACK_*_COMPONENT_INSTALL is on. The DEB generator overrides it either
# way and keeps its paths rooted at ./usr, which is what dpkg requires.
set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY 1)
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/CPackProjectConfig.cmake")

set(CPACK_PACKAGE_NAME gittop)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VENDOR "Parth Sinha")
set(CPACK_PACKAGE_CONTACT "Parth <sinhaparth555@gmail.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/sinhaparth5/gittop")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "btop-inspired terminal dashboard for Git, GitHub and GitLab")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)

# Release builds carry debug info that triples the download for nothing a user
# can act on. The sources are the debugger anyone would actually reach for.
set(CPACK_STRIP_FILES ON)

# The tarball name; the .deb takes DEB-DEFAULT below instead, because Debian
# names a package by its own architecture ("amd64") and CMAKE_SYSTEM_PROCESSOR
# says "x86_64", which would produce a file dpkg refuses to guess about.
set(CPACK_PACKAGE_FILE_NAME "gittop-${PROJECT_VERSION}-linux-${CMAKE_SYSTEM_PROCESSOR}")

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION devel)
set(CPACK_DEBIAN_PACKAGE_PRIORITY optional)

# dpkg-shlibdeps reads the built binary and works out the runtime packages from
# what it actually links, which is the only way to get libc and libcurl right
# for the distribution it was built on rather than guessing version numbers.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

# The one dependency shlibdeps cannot see. libgit2 is built USE_HTTPS=
# OpenSSL-Dynamic, which dlopen()s libssl at run time, so nothing in the binary
# references it and an https remote would fail at the first fetch with no clue
# why. Named explicitly and appended to what shlibdeps finds.
# The alternation covers the t64 transition: Ubuntu 24.04 and later renamed the
# package to libssl3t64, and a package depending only on the old name is
# uninstallable there for a reason the user cannot act on.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libssl3 | libssl3t64")

set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
"btop-inspired terminal dashboard for Git, GitHub and GitLab
 gittop shows the state of a Git repository in a terminal: working tree,
 history, branches, a commit graph, diffs and stashes, with no network and no
 account needed. Given either, it adds panels for the repository as the
 provider describes it, CI runs and pipelines, and open pull or merge requests.
 .
 It reads repositories through libgit2 rather than by parsing the output of
 git(1), and supports GitHub, GitLab and self-hosted instances of both.")

include(CPack)
