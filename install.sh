#!/bin/sh
#
# Installs or updates gittop from the latest GitHub release.
#
#   curl -fsSL https://raw.githubusercontent.com/sinhaparth5/gittop/master/install.sh | sh
#
# Running it again is the update: it does nothing when the installed copy is
# already the latest. Two variables change what it does:
#
#   GITTOP_PREFIX   where to install (default ~/.local, so bin/gittop lands in
#                   ~/.local/bin and nothing needs sudo)
#   GITTOP_VERSION  a release to install instead of the latest, e.g. 2026.10.1
#
# POSIX sh rather than bash, because `| sh` is dash on Debian and Ubuntu.
# Nothing the server sends is ever evaluated: the version comes out of a
# redirect URL and is checked against a pattern before it is used in a path.

set -eu

repo="sinhaparth5/gittop"
prefix="${GITTOP_PREFIX:-$HOME/.local}"

say() { printf 'gittop: %s\n' "$*"; }
die() { printf 'gittop: %s\n' "$*" >&2; exit 1; }

case "$(uname -s)-$(uname -m)" in
  Linux-x86_64 | Linux-amd64) ;;
  *) die "no prebuilt release for $(uname -s) $(uname -m) yet; see https://github.com/$repo#building-it" ;;
esac

for tool in curl tar sha256sum; do
  command -v "$tool" >/dev/null 2>&1 || die "needs $tool"
done

# The latest release is read from where /releases/latest redirects to, rather
# than from the API, which rate-limits anonymous callers to 60 requests an hour
# and would need a JSON parser.
version="${GITTOP_VERSION:-}"
if [ -z "$version" ]; then
  url=$(curl -fsSLI -o /dev/null -w '%{url_effective}' "https://github.com/$repo/releases/latest") ||
    die "could not reach github.com"
  version="${url##*/}"
fi
version="${version#v}"
case "$version" in
  [0-9][0-9][0-9][0-9].[0-9][0-9].[0-9]*) ;;
  *) die "could not work out the latest version (got '$version')" ;;
esac

bin="$prefix/bin/gittop"
if [ -x "$bin" ] && [ "$("$bin" --version 2>/dev/null)" = "gittop $version" ]; then
  say "$version is already installed at $bin"
  exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

asset="gittop-$version-linux-x86_64.tar.gz"
base="https://github.com/$repo/releases/download/v$version"
say "downloading $version"
curl -fsSL -o "$tmp/$asset" "$base/$asset" || die "could not download $asset"
curl -fsSL -o "$tmp/SHA256SUMS" "$base/SHA256SUMS" ||
  die "v$version publishes no SHA256SUMS, so it cannot be verified; 2026.10.1 is the first that does"

# Only the line for this asset, so a SHA256SUMS listing files we did not
# download does not fail the check.
(cd "$tmp" && grep "  $asset\$" SHA256SUMS | sha256sum -c --status) ||
  die "checksum mismatch for $asset; nothing was installed"

# The tarball is bin/, share/applications/, share/icons/ and share/doc/ under
# one top-level directory, which is exactly a prefix's layout.
mkdir -p "$prefix"
tar -xzf "$tmp/$asset" -C "$prefix" --strip-components=1
say "installed $version to $bin"

case ":$PATH:" in
  *":$prefix/bin:"*) ;;
  *) say "$prefix/bin is not on your PATH; add it, e.g. echo 'export PATH=\"$prefix/bin:\$PATH\"' >> ~/.profile" ;;
esac
other=$(command -v gittop 2>/dev/null || true)
if [ -n "$other" ] && [ "$other" != "$bin" ]; then
  say "note: '$other' comes first on your PATH; remove it (sudo apt remove gittop) or run $bin"
fi
