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
# CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY is global and the DEB generator
# does not override it, which is why this file exists rather than one set() in
# CMakeLists.txt.

if(CPACK_GENERATOR STREQUAL "DEB")
  set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY 0)
endif()
