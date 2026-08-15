# Per-generator CPack overrides. CPack includes this once per generator with
# CPACK_GENERATOR set, which is the only hook for a setting that has to differ
# between the .deb and the tarball.
#
# The setting in question is the top-level directory. The tarball wants one, so
# it unpacks into a folder of its own instead of emptying bin/ and share/ into
# whatever directory the user was standing in. The .deb must not have one: dpkg
# reads the archive paths as absolute install locations, so a top-level
# directory means every file lands in /gittop-2026.08.0-linux-x86_64/usr/...
# instead of /usr/... — a package that installs successfully and puts the binary
# somewhere no PATH will ever find it.
#
# NSIS must not have one either, for a reason that reads differently and ends in
# the same place. An installer already asks the user where to put things, so the
# extra version-stamped level is redundant on its face — but what it actually
# breaks is the one feature the installer exists for. CPack's NSIS template
# hardcodes `Push $INSTDIR\bin` for the add-to-PATH step, and `AddToPath` opens
# with `IfFileExists "$0\*.*" "" AddToPath_done`. With a top-level directory the
# binary lands in $INSTDIR\gittop-<version>-windows-x86_64\bin, so $INSTDIR\bin
# does not exist, so the function returns **without a word** and the installer
# reports success. That is issue 26: "Add to PATH for all users" ticked, PATH
# unchanged, no error anywhere, and every user editing their environment by
# hand. It fails the same way for the current-user choice; that one had simply
# not been tried. The same $INSTDIR\bin assumption is what DisplayIcon is
# written against, so the Programs and Features entry loses its icon too.
#
# CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY is global and neither generator
# overrides it, which is why this file exists rather than one set() in
# CMakeLists.txt. The archive generators are the only ones that want it, and
# they are the ones left alone here.

if(CPACK_GENERATOR STREQUAL "DEB" OR CPACK_GENERATOR STREQUAL "NSIS")
  set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY 0)
endif()
