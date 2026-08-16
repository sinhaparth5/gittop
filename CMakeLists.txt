cmake_minimum_required(VERSION 3.24)

# CalVer, YYYY.MM.PATCH. The month keeps its leading zero — CMake preserves it
# through PROJECT_VERSION, so "2026.08.0" survives to the binary rather than
# arriving as "2026.8.0" and disagreeing with the tag it was built from.
#
# This is the only place the version is written. It reaches the program as
# GITTOP_VERSION below, so `--version`, the settings page and the splash cannot
# drift from each other or from the release; the workflow in
# .github/workflows/release.yml refuses a tag that does not match it.
project(gittop VERSION 2026.08.9 LANGUAGES C CXX)

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

# The HTTPS backend is the one libgit2 choice that differs by platform, and it
# differs in order to keep the property that made OpenSSL-Dynamic right here:
# no build dependency, and the platform's own certificate store.
#
#   WinHTTP  is libgit2's native Windows backend. It verifies against the
#            Windows certificate store, which is the store the machine's
#            administrator actually manages, and it links against a DLL that
#            ships with the operating system. OpenSSL-Dynamic would work but
#            would dlopen a libssl that is not there on a stock Windows.
#
# `exec` needs no such split: libgit2 has a full CreateProcess implementation of
# git_process, so the ssh transport runs Windows OpenSSH's ssh.exe and inherits
# the user's ~/.ssh/config, agent and known_hosts exactly as it does elsewhere.
if(WIN32)
  set(USE_HTTPS       "WinHTTP"          CACHE STRING "" FORCE)
else()
  set(USE_HTTPS       "OpenSSL-Dynamic"  CACHE STRING "" FORCE)
endif()
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

# ------------------------------------------------------------------ curl
# Taken from the system rather than vendored, *where there is a system copy*.
# libcurl is one of the very few libraries where the distribution's build is the
# one you want: it carries the platform's CA bundle and its TLS backend, and a
# self-built copy would have to be told where the system's certificates live
# before it could verify anything.
#
# Windows has no system libcurl, so there it is fetched — and the objection
# above dissolves rather than being accepted, because Schannel *is* the
# platform's TLS backend and verifies against the Windows certificate store.
# There is no CA bundle to locate and none is shipped. HTTP_ONLY drops every
# protocol gittop does not speak, which is all of them but HTTP and HTTPS.
if(WIN32)
  set(BUILD_CURL_EXE      OFF CACHE BOOL "" FORCE)
  set(BUILD_LIBCURL_DOCS  OFF CACHE BOOL "" FORCE)
  set(BUILD_MISC_DOCS     OFF CACHE BOOL "" FORCE)
  set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
  set(ENABLE_CURL_MANUAL  OFF CACHE BOOL "" FORCE)
  set(HTTP_ONLY           ON  CACHE BOOL "" FORCE)
  set(CURL_USE_SCHANNEL   ON  CACHE BOOL "" FORCE)
  set(CURL_USE_OPENSSL    OFF CACHE BOOL "" FORCE)
  set(CURL_USE_LIBPSL     OFF CACHE BOOL "" FORCE)
  set(CURL_USE_LIBSSH2    OFF CACHE BOOL "" FORCE)
  set(CURL_ZLIB           OFF CACHE STRING "" FORCE)
  set(CURL_BROTLI         OFF CACHE BOOL "" FORCE)
  set(CURL_ZSTD           OFF CACHE BOOL "" FORCE)
  set(USE_LIBIDN2         OFF CACHE BOOL "" FORCE)
  set(USE_NGHTTP2         OFF CACHE BOOL "" FORCE)

  FetchContent_Declare(curl
    GIT_REPOSITORY https://github.com/curl/curl.git
    GIT_TAG        curl-8_11_1
    GIT_SHALLOW    TRUE)

  FetchContent_MakeAvailable(ftxui libgit2 nlohmann_json curl)
else()
  FetchContent_MakeAvailable(ftxui libgit2 nlohmann_json)
  find_package(CURL 7.68 REQUIRED)
endif()

# ---------------------------------------------------------------- gittop
# Exactly one of these is compiled, chosen here rather than by an #ifdef inside a
# file that contains both. A file that compiles on both platforms but only ever
# runs on one is a file whose dead half rots without anyone noticing — which is
# what happened to the nerd glyph set for three releases.
if(WIN32)
  set(GITTOP_PLATFORM_SOURCE src/platform/windows.cpp)

  # The icon and the version block, compiled into the executable. There is no
  # freedesktop .desktop file or hicolor icon directory here to install them
  # into: on this platform that metadata lives inside the binary, and one
  # without it shows blank fields in every properties dialog.
  #
  # Configured rather than checked in, so the version comes from
  # project(gittop VERSION ...) like everything else and cannot drift from what
  # `--version` prints. windres wants a forward-slashed absolute path.
  set(GITTOP_VERSION       ${PROJECT_VERSION})
  set(GITTOP_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
  set(GITTOP_VERSION_MINOR ${PROJECT_VERSION_MINOR})
  set(GITTOP_VERSION_PATCH ${PROJECT_VERSION_PATCH})
  set(GITTOP_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/gittop.ico")
  configure_file(packaging/gittop.rc.in "${CMAKE_CURRENT_BINARY_DIR}/gittop.rc" @ONLY)
  list(APPEND GITTOP_PLATFORM_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/gittop.rc")
else()
  set(GITTOP_PLATFORM_SOURCE src/platform/posix.cpp)
endif()

add_executable(gittop
  ${GITTOP_PLATFORM_SOURCE}
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

if(MSVC)
  # /W4 is the closest thing MSVC has to -Wall -Wextra; its own /Wall includes
  # several thousand diagnostics about the standard library's headers.
  # /utf-8 is not optional: the sources contain UTF-8 string literals — every
  # glyph in the unicode set — and without it MSVC reads them in the machine's
  # ANSI code page and produces mojibake with no diagnostic.
  target_compile_options(gittop PRIVATE /W4 /utf-8 /permissive-)

  # _CRT_SECURE_NO_WARNINGS silences C4996 on std::getenv, which gittop calls in
  # seven files to read TERM, COLORTERM, XDG_*, HOME and the token variables.
  #
  # This is not waving away a real warning. std::getenv is standard C++ and is
  # not going anywhere; the deprecation is Microsoft's own, and the suggested
  # replacement, _dupenv_s, allocates a copy the caller must free and exists on
  # no other platform — so taking the advice would mean an #ifdef in every one
  # of those files to avoid a diagnostic that describes no defect here. The
  # thread-safety concern behind it is real but is about *setting* the
  # environment while reading it, which gittop does only from the UI thread with
  # no transfer in flight (see platform::SetEnv).
  target_compile_definitions(gittop PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(gittop PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_compile_definitions(gittop PRIVATE GITTOP_VERSION="${PROJECT_VERSION}")

if(WIN32)
  # WIN32_LEAN_AND_MEAN and NOMINMAX are set on the target rather than at the top
  # of windows.cpp so that every translation unit which pulls windows.h in
  # transitively gets the same one. NOMINMAX in particular has to be global:
  # windows.h defines min and max as macros, and they collide with <algorithm>
  # in most of this codebase.
  #
  # _WIN32_WINNT 0x0601 is Windows 7, which is the floor for CancelIoEx and
  # FILE_FLAG_FIRST_PIPE_INSTANCE. gittop needs a far newer console than that to
  # be usable, but there is no reason for the headers to be the thing that says so.
  target_compile_definitions(gittop PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0601)

  # advapi32  the token and ACL calls behind platform::RestrictToOwner
  # shell32   ShellExecuteW, which is how a URL reaches the user's browser
  # ws2_32/crypt32/bcrypt/winhttp/rpcrt4/secur32 are libgit2's and libcurl's,
  # and are listed here because a static build of either does not carry its own
  # dependencies with it.
  target_link_libraries(gittop PRIVATE
    advapi32 shell32 ws2_32 crypt32 bcrypt winhttp rpcrt4 secur32 ole32 user32)
endif()

# ---------------------------------------------------------------- install
include(GNUInstallDirs)

install(TARGETS gittop RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT gittop)

# The desktop entry and the icon theme are freedesktop mechanisms, and there is
# no Windows equivalent to port them to: a Start menu shortcut is made by the
# installer rather than shipped as a file, so it lives in the NSIS block below.
if(NOT WIN32)
  # A TUI still earns a menu entry — btop ships one — and Terminal=true is the
  # line that makes a launcher open a terminal for it rather than starting it
  # headless, where it would draw its alternate screen into nothing.
  install(FILES packaging/gittop.desktop
          DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
          COMPONENT gittop)

  # The scalable icon is what current desktops prefer and the one that stays
  # sharp at whatever size a panel asks for; the rasters below are for the
  # environments that will not render SVG. Both live under hicolor, which is the
  # theme every other theme falls back to, so the icon resolves regardless of
  # what the user has selected.
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
endif()

# CMAKE_INSTALL_DOCDIR is share/doc/gittop, which is a Unix layout. On Windows
# the package root *is* the application directory, so the docs go beside the
# executable where somebody might actually open them.
if(WIN32)
  install(FILES README.md CHANGELOG.md LICENSE DESTINATION . COMPONENT gittop)
else()
  install(FILES README.md CHANGELOG.md LICENSE
          DESTINATION ${CMAKE_INSTALL_DOCDIR}
          COMPONENT gittop)
endif()

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
#
# Every generator, Windows included. It is tempting to skip this there on the
# theory that libgit2's install rules are a Unix concern — they are not, and the
# zip built without this line contained the whole of include/git2/**. The
# symptom is the same on both platforms and so is the fix.
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_NSIS_COMPONENT_INSTALL ON)
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
if(WIN32)
  set(CPACK_PACKAGE_FILE_NAME "gittop-${PROJECT_VERSION}-windows-x86_64")
else()
  set(CPACK_PACKAGE_FILE_NAME "gittop-${PROJECT_VERSION}-linux-${CMAKE_SYSTEM_PROCESSOR}")
endif()

# -------------------------------------------------------- windows packaging
# Two artifacts, because they answer two different questions.
#
#   ZIP   unpack and run. This is the one that works with no administrator, no
#         installer prompt and no uninstall entry, which is what most people
#         want from a single-binary terminal tool — and it is the only one that
#         can be used from a locked-down machine.
#
#   NSIS  an installer, which exists for the one thing the zip cannot do: put
#         gittop on PATH. A terminal program that is not on PATH is a terminal
#         program you cannot type the name of, and telling every Windows user to
#         edit their environment variables by hand is not a port, it is a README
#         apologising for one.
if(WIN32)
  set(CPACK_NSIS_PACKAGE_NAME "gittop")
  set(CPACK_NSIS_DISPLAY_NAME "gittop ${PROJECT_VERSION}")
  set(CPACK_PACKAGE_INSTALL_DIRECTORY "gittop")
  set(CPACK_NSIS_URL_INFO_ABOUT "${CPACK_PACKAGE_HOMEPAGE_URL}")
  set(CPACK_NSIS_HELP_LINK "${CPACK_PACKAGE_HOMEPAGE_URL}")
  set(CPACK_NSIS_CONTACT "${CPACK_PACKAGE_CONTACT}")

  # The whole reason the installer exists — it adds an "add to PATH" page.
  set(CPACK_NSIS_MODIFY_PATH ON)

  # ...and the page's write is not safe on its own. CPack's AddToPath silently
  # replaces a PATH it could not read, which is any PATH over NSIS's 1024-char
  # string limit. packaging/gittop-path.nsh has the measurements; it refuses the
  # unsafe cases and tells the user what to add by hand.
  #
  # !include'd rather than inlined. Reading the file into the variable is the
  # obvious move and does not survive the trip: every CPACK_* setting is written
  # back out as a CMake string in CPackConfig.cmake, so the script's backslashes
  # become escape sequences ("Manager\Environment" -> invalid \E) and its
  # ${HIVE} becomes an empty variable expansion. A path has neither problem.
  #
  # The path has to be native, and that is not a stylistic preference. NSIS on
  # Windows resolves !include itself rather than handing the string to the CRT,
  # and it does not accept forward slashes: makensis answers
  # `!include: could not find: "D:/a/gittop/gittop/packaging/gittop-path.nsh"`
  # and CPack reports only "Problem running NSIS command". The Linux makensis
  # normalises the separator, so a cross-build packages perfectly and the
  # failure lands on a tag — which is where it landed. Every other path in the
  # generated script keeps its forward slashes and works, because OutFile,
  # File and the icons all go through the file system rather than the
  # preprocessor; !include is the one that is strict.
  #
  # TO_NATIVE_PATH is therefore what decides, and it is a no-op on Linux, where
  # the string stays exactly as it was.
  file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/packaging/gittop-path.nsh"
       gittop_path_nsh)

  # Which brings back the escaping this comment block used to say was avoidable.
  # CPack writes every CPACK_* value into CPackConfig.cmake verbatim, between
  # quotes it adds itself and does not escape, and cpack then parses that file
  # as CMake. So each backslash has to arrive there doubled or it is read as an
  # escape sequence — "D:\a\gittop" is a hard error on \a, the same way a
  # file(READ) of the script itself dies on "Manager\Environment".
  string(REPLACE "\\" "\\\\" gittop_path_nsh "${gittop_path_nsh}")

  # The tripled backslashes on the quotes are the same round trip: a plain \"
  # arrives as a bare quote that closes the string early and truncates the
  # setting. The quotes have to be there at all because an installer built from
  # a checkout under "C:\Users\Some Name" has a space in this path.
  #
  # EXTRA_INSTALL_COMMANDS and not CPACK_NSIS_DEFINES, which is the hook that
  # looks right: it is the only global-scope one in the template, it is
  # undocumented, and the generator overwrites it. The symptom is that the
  # setting simply arrives empty.
  set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS
      "  !include \\\"${gittop_path_nsh}\\\"")

  # Upgrading over an existing install rather than stacking a second copy beside
  # it, which is what leaves two entries in Programs and Features and a stale
  # gittop.exe earlier on PATH than the new one.
  set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)

  set(CPACK_NSIS_MUI_ICON   "${CMAKE_CURRENT_SOURCE_DIR}/assets/gittop.ico")
  set(CPACK_NSIS_MUI_UNIICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/gittop.ico")
  set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\gittop.exe")

  # NSIS reads the license page from a file it expects to be text. LICENSE has
  # no extension, and some NSIS builds refuse it on that basis alone, so it is
  # copied to a name they all accept rather than renamed in the repository.
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
                 "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt" COPYONLY)
  set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt")
endif()

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
