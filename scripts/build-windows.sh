#!/usr/bin/env bash
#
# Builds gittop.exe from a Linux checkout, and optionally runs it under Wine.
#
# The release artifact is built by CI on Windows with MSVC — see the windows job
# in .github/workflows/release.yml. This script is the other thing: a way to
# find out whether the Windows build still compiles without pushing a tag to ask.
# There is no test target in this project and CI runs no tests, so nothing else
# will tell you that a change broke the port.
#
# Everything happens inside a container, so the only requirement on the machine
# running this is docker. mingw-w64 rather than MSVC because MSVC does not run
# here; the two disagree about enough (warning flags, the CRT) that a clean
# build under this one is evidence and not proof. It catches the mistakes that
# actually get made — a POSIX header that crept back into a shared file, a Win32
# call with the wrong argument — which is the bulk of them.
#
# usage: scripts/build-windows.sh [--run] [--package]
set -euo pipefail

cd "$(dirname "$0")/.."

image=gittop-win
build_dir=build-win
run_it=0
package_it=0

for arg in "$@"; do
  case "$arg" in
    --run)     run_it=1 ;;
    --package) package_it=1 ;;
    *) echo "usage: $0 [--run] [--package]" >&2; exit 2 ;;
  esac
done

if ! docker image inspect "$image" >/dev/null 2>&1; then
  echo "==> building the $image toolchain image (first run only)"
  docker build -t "$image" -f packaging/Dockerfile.windows packaging
fi

echo "==> configuring"
docker run --rm -v "$PWD:/src" -w /src "$image" \
  cmake -B "$build_dir" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/src/cmake/mingw-w64-x86_64.cmake \
        -DCMAKE_BUILD_TYPE=Release

echo "==> building"
docker run --rm -v "$PWD:/src" -w /src "$image" \
  bash -c "cmake --build $build_dir -j\$(nproc)"

ls -l "$build_dir/gittop.exe"

if [ "$package_it" = 1 ]; then
  echo "==> packaging"
  # Both generators, because they fail in different ways: the zip is a plain
  # archive of whatever got installed, while NSIS runs a real installer script
  # and is the one that notices a missing icon or an unreadable license file.
  docker run --rm -v "$PWD:/src" -w "/src/$build_dir" "$image" cpack -G ZIP
  docker run --rm -v "$PWD:/src" -w "/src/$build_dir" "$image" cpack -G NSIS
  ls -l "$build_dir"/*.zip "$build_dir"/gittop-*windows*.exe

  # The check that actually matters here, and the one that has already failed
  # once: CPACK_COMPONENTS_ALL is what keeps libgit2's own install rules out of
  # the package, and without it the zip carries the whole of include/git2/**.
  # It is a silent failure — a package that installs fine and is twice the size.
  echo "==> checking the package carries nothing but gittop"
  docker run --rm -v "$PWD:/src" -w "/src/$build_dir" "$image" python3 -c '
import glob, sys, zipfile
names = zipfile.ZipFile(sorted(glob.glob("*.zip"))[0]).namelist()
strays = [n for n in names if "/include/" in n or n.endswith((".a", ".pc", ".lib"))]
print("\n".join("  " + n for n in names))
if strays:
    print("\nstray development files in the package:", file=sys.stderr)
    print("\n".join("  " + s for s in strays[:10]), file=sys.stderr)
    sys.exit(1)
print("\nclean")
'
fi

if [ "$run_it" = 1 ]; then
  echo "==> running under wine"
  # WINEDEBUG silences the fixme: chatter, which is voluminous and none of it is
  # about gittop. A fresh WINEPREFIX per run keeps this from depending on
  # whatever a previous run left in ~/.wine.
  docker run --rm -v "$PWD:/src" -w /src -e WINEDEBUG=-all "$image" \
    bash -c "export WINEPREFIX=/tmp/wine && wineboot -i >/dev/null 2>&1;
             wine $build_dir/gittop.exe --version"
fi
