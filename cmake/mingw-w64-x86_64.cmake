# Cross-compiling toolchain for x86_64 Windows with mingw-w64.
#
# This exists so the Windows build can be exercised from a Linux machine. The
# release artifact is built by CI on Windows with MSVC, but a port that nobody
# can compile without pushing a tag is a port that breaks silently between tags
# — and this repository has no test target, so the only thing that ever catches
# a break is somebody building it. With this file and Wine, gittop.exe can be
# built and run from an ordinary checkout.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# libstdc++, libgcc and the winpthread runtime are the only things gittop.exe
# would otherwise need beside it. Linking them in is what makes the artifact a
# single file that runs on a machine with nothing installed, which is the
# baseline expectation on this platform.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++ -static")
