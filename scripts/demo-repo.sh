#!/usr/bin/env bash
# Builds the throwaway repository that demo.tape is recorded against.
#
#   scripts/demo-repo.sh [destination]     # default: demo-repo/
#
# Checked in for the same reason demo.tape is: the GIF is only reproducible if
# the thing it was recorded against is reproducible too. Recording against
# gittop's own repository does not work — it has a couple of dozen commits over
# a few days, so the activity heatmap is one cluster at the right edge and the
# graph is a spike. Both are among the frames worth having.
#
# Nothing here is anybody's real repository. Cloning one would put a stranger's
# commit messages and authors into an image on the README, and it would need the
# network, which the tape deliberately does not.
#
# Dates are relative to today rather than fixed. A hardcoded window would drift
# out of the heatmap's trailing thirteen weeks and the demo would slowly become
# a picture of an abandoned project. That costs byte-identical reproducibility
# and buys a recording that still looks right next year.
set -euo pipefail

dest=${1:-demo-repo}

# The bare repository that becomes `origin` sits beside the destination, so both
# have to be clear. Refusing rather than deleting: this takes a path from the
# command line and removing a directory somebody named by mistake is not a thing
# a helper script gets to do.
for path in "$dest" "${dest%/}.git"; do
  if [ -e "$path" ]; then
    echo "$path already exists — remove it first:" >&2
    echo "  rm -rf ${dest%/} ${dest%/}.git" >&2
    exit 1
  fi
done

# Seeded so the shape of the history (which days are busy, which files a commit
# touches) is the same every run. Only the absolute dates move.
RANDOM=20260814

readonly DAYS=180
readonly NOW=$(date +%s)
readonly DAY=86400

at() { date -u -d "@$1" --iso-8601=seconds; }

# Weekday-heavy with quiet stretches, because a heatmap where every cell is the
# same shade shows nothing.
commits_for_day() {
  local ago=$1 dow
  dow=$(date -u -d "@$((NOW - ago * DAY))" +%u)

  # Two deliberate lulls, so the graph has a shape rather than a plateau.
  if [ "$ago" -gt 120 ] && [ "$ago" -lt 134 ]; then echo 0; return; fi
  if [ "$ago" -gt 58 ] && [ "$ago" -lt 67 ]; then echo 0; return; fi

  if [ "$dow" -ge 6 ]; then
    [ $((RANDOM % 5)) -eq 0 ] && echo 1 || echo 0
  else
    echo $((RANDOM % 5))
  fi
}

authors=(
  "Ada Iyer <ada@example.invalid>"
  "Ruth Nakamura <ruth@example.invalid>"
  "Sam Okafor <sam@example.invalid>"
)

# "fix" appears in several because demo.tape types it into the `/` filter and a
# filter that matches nothing is a poor advertisement for the filter.
subjects=(
  "fix the retry budget so a 429 does not spin"
  "carry the request id through the transport"
  "drop the second copy of the header table"
  "fix a torn read when the buffer wraps"
  "measure the parse in cells rather than bytes"
  "explain why the timeout is where it is"
  "hoist the allocation out of the loop"
  "fix the off-by-one in the window size"
  "teach the config reader about comments"
  "stop logging the token, even masked"
  "name the error instead of returning -1"
  "widen the test to cover an empty body"
  "fix a leak on the error path"
  "cache the resolved endpoint per host"
  "trim the trailing slash once, at the edge"
  "make the poll interval configurable"
  "fix the ordering of the shutdown steps"
  "shrink the struct by reordering it"
  "prefer the arena for short-lived nodes"
  "document the two-phase handshake"
)

# Each branch owns its own files. The two branches that get merged therefore
# never touch a file master touched in the meantime, so `merge --no-ff` cannot
# stop on a conflict halfway through generating the history.
trunk_files=(src/transport.c src/parser.c src/session.c include/orbit.h)
branch_file=""
branch=master

git init -q -b master "$dest"
cd "$dest"
git config user.name "Ada Iyer"
git config user.email "ada@example.invalid"
git config commit.gpgsign false

mkdir -p src include

cat > README.md <<'DOC'
# orbit

A small HTTP client toolkit. Not a real project — this repository exists so the
gittop demo has something with history in it to draw.
DOC

seed_file() {
  {
    echo "/* ${1##*/} */"
    for i in $(seq 1 30); do echo "int line_$i = $i;"; done
  } > "$1"
}

for f in "${trunk_files[@]}"; do seed_file "$f"; done
seed_file src/config.c

n=0
mut=0
commit_at() {
  local ago=$1 msg=$2 author=$3 f
  local when=$((NOW - ago * DAY + (RANDOM % 28800) + 32400))

  if [ -n "$branch_file" ]; then
    f=$branch_file
  else
    f=${trunk_files[$((RANDOM % ${#trunk_files[@]}))]}
  fi

  n=$((n + 1))

  # Several lines per commit, not one. A one-line change renders as a diff panel
  # that is four rows of content and the rest empty space, and the diff is one
  # of the views the GIF exists to show.
  local edits=$((RANDOM % 5 + 2))
  local e line
  for e in $(seq 1 "$edits"); do
    # 1000 + mut, not a random value: the seeded files hold 1..30 and mut never
    # repeats, so every commit is guaranteed to change something. A replacement
    # that happened to match what was already there left git with nothing to
    # commit and killed the run under `set -e`.
    mut=$((mut + 1))
    line=$((RANDOM % 30 + 1))
    sed -i "${line}s/.*/int line_${line} = $((1000 + mut));/" "$f"
  done

  # Every so often, added lines rather than changed ones, so the diff has green
  # hunks with no red in them too.
  if [ $((n % 6)) -eq 0 ]; then
    mut=$((mut + 1))
    printf 'static int helper_%d(void) { return %d; }\n' "$mut" "$mut" >> "$f"
  fi

  git add -A
  GIT_AUTHOR_DATE="$(at "$when")" GIT_COMMITTER_DATE="$(at "$when")" \
    git commit -q --author="$author" -m "$msg"
}

git add -A
GIT_AUTHOR_DATE="$(at $((NOW - DAYS * DAY)))" \
GIT_COMMITTER_DATE="$(at $((NOW - DAYS * DAY)))" \
  git commit -q -m "first commit"

open_branch() {
  branch=$1
  branch_file=$2
  git checkout -q -b "$branch"
  seed_file "$branch_file"
  git add -A
  GIT_AUTHOR_DATE="$(at "$3")" GIT_COMMITTER_DATE="$(at "$3")" \
    git commit -q -m "start ${branch#*/}"
  n=$((n + 1))
}

merge_branch() {
  local from=$1 when=$2
  git checkout -q master
  branch=master
  branch_file=""
  GIT_AUTHOR_DATE="$(at "$when")" GIT_COMMITTER_DATE="$(at "$when")" \
    git merge -q --no-ff "$from" -m "merge ${from#*/} into master"
}

# ------------------------------------------------------------------ the trunk
#
# Two feature branches are cut and merged with --no-ff, and a third is left
# open. While a branch is open the days alternate between it and master, so the
# two actually diverge — that is what puts more than one lane in the history
# gutter. A branch that is cut, committed to, and merged with master untouched
# draws a single straight line and the lane assignment has nothing to show.
for ago in $(seq $((DAYS - 1)) -1 0); do
  when=$((NOW - ago * DAY))

  case $ago in
    140) open_branch feature/streaming src/streaming.c "$when" ;;
    118) merge_branch feature/streaming "$when" ;;
    84)  open_branch feature/tls src/tls.c "$when" ;;
    52)  merge_branch feature/tls "$when" ;;
    # Left unmerged on purpose: the branches view is more interesting when
    # something in it is actually ahead.
    21)  open_branch refactor/config src/refactor_config.c "$when" ;;
    12)  git checkout -q master; branch=master; branch_file="" ;;
  esac

  # Alternate between the open branch and master on odd days, so both advance.
  if [ -n "$branch_file" ] && [ $((ago % 2)) -eq 1 ]; then
    git checkout -q master
    held=$branch_file
    branch_file=""
  else
    held=""
  fi

  count=$(commits_for_day "$ago")
  for _ in $(seq 1 "$count"); do
    commit_at "$ago" "${subjects[$((RANDOM % ${#subjects[@]}))]}" \
              "${authors[$((RANDOM % ${#authors[@]}))]}"
  done

  if [ -n "$held" ]; then
    git checkout -q "$branch"
    branch_file=$held
  fi
done

git checkout -q master

# ---------------------------------------------------------------- the upstream
#
# A bare repository beside this one, added as `origin` and pushed to. It is a
# plain path, so nothing here reaches the network — but it is a real remote as
# far as libgit2 is concerned, which is what the branches view and the status
# sidecar need. Without it every branch reads "no upstream" and the ahead/behind
# column, which is most of why those panels are worth a frame, is empty.
#
# The remote, CI and pull tabs are still not worth visiting on this repository:
# a filesystem path has no provider to detect. The tape does not go there.
bare="$PWD/../${dest##*/}.git"
git init -q --bare "$bare"
git remote add origin "$bare"
for b in master feature/streaming feature/tls refactor/config; do
  git push -q -u origin "$b"
done

# master alone moves on after the push, so it is ahead of its upstream and the
# others are level. A column where every row reads 0/0 says nothing.
git checkout -q master
for i in 3 2 1; do
  commit_at "$i" "${subjects[$((RANDOM % ${#subjects[@]}))]}" "${authors[0]}"
done

# --------------------------------------------------------------- the end state
#
# The status view is the first thing on screen and the tape stages a file from
# it, so it needs all four columns populated rather than a clean tree.
# Stashes first, and each one against a file the working-tree state below does
# not use. `git stash push <path>` takes whatever is in that path — staged or
# not — so stashing a file that is meant to end up in the staged column quietly
# empties it.
printf 'int stashed = 5;\n' >> src/session.c
git stash push -q -m "half-finished session teardown" src/session.c
printf 'int wip_streaming = 6;\n' >> src/streaming.c
git stash push -q -m "streaming backpressure, do not ship" src/streaming.c
printf 'int wip_tls = 7;\n' >> src/tls.c
git stash push -q -m "tls session resumption experiment" src/tls.c

# Then the working tree the status view opens on: something in staged, unstaged
# and untracked, since that is the first frame of the GIF and the tape stages a
# file out of it.
printf 'int staged_change = 1;\n' >> src/transport.c
printf 'int also_staged = 2;\n' >> include/orbit.h
git add src/transport.c include/orbit.h

printf 'int unstaged_change = 3;\n' >> src/parser.c
printf 'int more_unstaged = 4;\n' >> src/config.c

printf 'scratch\n' > notes.txt

echo "built $dest"
echo "  commits on master: $(git rev-list --count HEAD)"
echo "  total objects:     $(git rev-list --count --all)"
echo "  span:              $(git log --reverse --format=%ad --date=short | head -1) .. $(git log -1 --format=%ad --date=short)"
echo "  stashes:           $(git stash list | wc -l)"
echo "  upstream:          $(git rev-list --count '@{u}..HEAD') ahead of $bare"
git --no-pager branch -vv
