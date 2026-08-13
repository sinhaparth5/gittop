#!/usr/bin/env bash
#
# Prints the body of one CHANGELOG.md section, for use as a release description.
#
#   scripts/changelog-section.sh 2026.08.0
#   scripts/changelog-section.sh v2026.08.0 path/to/CHANGELOG.md
#
# A leading "v" is accepted on either side, so the git tag can carry one and the
# changelog heading need not.
#
# This lives here rather than inline in the workflow so that it can be run
# before tagging. A release body is the one thing a workflow cannot check for
# you — an extractor that silently returns nothing produces a published, empty
# release, and the only way to test an inline script is to push a tag and find
# out. Exiting non-zero on an empty section is the whole point of the file.

set -euo pipefail

version="${1:-}"
file="${2:-CHANGELOG.md}"

if [[ -z "$version" ]]; then
  echo "usage: $(basename "$0") <version> [changelog]" >&2
  exit 2
fi

if [[ ! -f "$file" ]]; then
  echo "$(basename "$0"): no such file: $file" >&2
  exit 2
fi

want="${version#v}"

# Everything after the matching "## <version>" heading, up to the next "## ".
# Headings are "## <version> — <date>", so the version is the second field.
section=$(
  awk -v want="$want" '
    /^## / {
      if (found) { exit }
      tag = $2
      sub(/^v/, "", tag)
      if (tag == want) { found = 1; next }
    }
    found { print }
  ' "$file"
)

# Drop leading and trailing blank lines, so the release body neither starts with
# whitespace nor trails the separator that precedes the next section.
section=$(printf '%s\n' "$section" | awk '
  { lines[NR] = $0 }
  END {
    first = 1
    last = NR
    while (first <= NR && lines[first] ~ /^[[:space:]]*$/) { first++ }
    while (last >= first && lines[last] ~ /^[[:space:]]*$/) { last-- }
    for (i = first; i <= last; i++) { print lines[i] }
  }
')

if [[ -z "$section" ]]; then
  echo "$(basename "$0"): no changelog section for $want in $file" >&2
  echo "versions found:" >&2
  awk '/^## / { print "  " $2 }' "$file" >&2
  exit 1
fi

printf '%s\n' "$section"
