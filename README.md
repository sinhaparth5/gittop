<div align="center">

<img src="assets/logo.svg" width="104" alt="">

# gittop

**A btop-style terminal dashboard for Git and CI.**

<p>
  <img src="https://img.shields.io/badge/status-pre--alpha-f0883e?style=flat-square" alt="Status: pre-alpha">
  <img src="https://img.shields.io/badge/license-GPL--3.0-3fb950?style=flat-square" alt="License: GPL-3.0">
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&logo=cplusplus&logoColor=white" alt="C++20">
  <img src="https://img.shields.io/badge/build-CMake-064F8C?style=flat-square&logo=cmake&logoColor=white" alt="Built with CMake">
  <img src="https://img.shields.io/badge/TUI-FTXUI-8957e5?style=flat-square" alt="TUI: FTXUI">
  <img src="https://img.shields.io/badge/git-libgit2-f05033?style=flat-square&logo=git&logoColor=white" alt="Git: libgit2">
  <img src="https://img.shields.io/badge/platform-Linux-333e58?style=flat-square&logo=linux&logoColor=white" alt="Platform: Linux">
</p>

</div>

---

## About gittop

Most Git tooling makes you choose. A TUI like lazygit shows you the repo but knows nothing about
your pipeline. The GitHub web UI shows you the pipeline but pulls you out of the terminal. gittop
puts both in one screen: working tree state on the left, commit history and activity graphs in the
middle, CI runs and open pull requests on the right.

The local half works with no network. libgit2 reads the repo directly, so staging a file or writing
a commit does not shell out to `git` or depend on its output format. The remote half talks to the
GitHub and GitLab REST APIs and degrades to the local view when there is no token, no remote, or no
connection.

> [!NOTE]
> Everything on this page runs today except the things listed under [Planned](#planned):
> repository state, diffs, stashes, rebase helpers, filtering, CI, pull requests, push and pull,
> themes, custom layouts and the mouse all work. What is left is the visual design pass. See
> [Current status](#current-status) for the full picture.

## What works today

Nine views, switched with `1` through `9` or cycled with `tab`.

**Status.**

- Status cards for staged, unstaged, untracked, and conflicted counts, with gradient fill bars
  that ease to their new value when something changes
- A file list grouped by state, with the directory dimmed and the filename bright so long lists
  scan by name
- Stage and unstage per file (`space`, or `s` and `u`), or stage everything with `a`
- Discard, behind a confirm dialog. Tracked files restore from the index, matching `git restore`;
  untracked files are deleted, and the dialog says which of the two is about to happen
- Commit from an overlay, written through `git_commit_create`
- Keybinding help on `?`

Status is conveyed by glyph and letter as well as color, so a row reads correctly without being
able to separate green from amber.

**History.**

- Commit log with a box-drawing lane graph, branch and tag badges, author, and relative age
- A 91-day activity heatmap, 13 weeks across by weekday down
- HEAD marked distinctly from every other commit

**Branches.**

- Local branches with ahead/behind counts against their upstream, checked-out branch first

**Graph.**

- A braille area chart of commits over the whole history, gradient filled with a bright crest
- Pan through time with `h` and `l`, jump to either end with `g` and `G`
- Switch granularity between day, week, and month with `d`, `w`, and `m`
- A scroll handle shows where the visible window sits in the full timeline
- Top authors, weekday distribution, and a commits-by-hour sparkline in the author's own timezone

**Diff.**

- The unstaged diff, the staged diff, or any commit's, with old and new line numbers side by side
- `enter` on a file in the status view opens that file's diff; `enter` on a commit in the history
  view opens that commit's, against its first parent
- `s` swaps between the unstaged and staged halves; `[` and `]` jump between files
- Per-file additions and deletions on the banner, and a total in the header
- A new file shows its contents rather than announcing that a file appeared and stopping there;
  a binary file is named rather than dumped; a rename is drawn as a rename
- Capped at 20,000 lines, and it says where it stopped rather than trailing off

**Stashes.**

- `S` puts the whole working tree away, untracked files included — a "clean" tree with new files
  still in it is not what anyone means by stashing their work
- The list shows each entry's index, the branch it came from, its subject and its age
- `a` applies and keeps the entry, `p` pops and `d` drops, the last two behind a confirm
- Apply and pop are `GIT_CHECKOUT_SAFE`, so one that would write over an uncommitted edit stops
  and says which file is in the way, and pop keeps the entry when it cannot apply cleanly

**Rebase and interrupted work.**

- `B` rebases the current branch onto its upstream, behind a confirm. It refuses on a dirty tree
  rather than stashing behind your back
- A merge, rebase, cherry-pick, revert or bisect left half-finished gets a banner above the file
  list saying which it is and, for a rebase, how far through
- `o` opens it: continue, or abort. Both go through a second confirm, so neither is one keystroke
- Continue commits what is staged and carries on to the next conflict; abort puts the branch back
  exactly where it started
- Committing during a merge writes every parent and clears the merge state, which is the part
  that is easy to get silently wrong

**Filtering.**

- `/` narrows whichever list is on screen — files, commits, branches, stashes, CI runs, pulls
- The box counts matches as you type, so a query that matches nothing says so before you finish
  the word rather than showing you a blank list
- `enter` keeps it and `esc` clears it; the footer keeps showing it, because a list hiding nine
  rows out of ten and a list with one row in it look identical otherwise

**Remote.**

- Reads the repository from GitHub or GitLab: description, default branch, visibility, last push
- Stars, forks, open issues, and watchers
- Remaining API budget as a bar, so you can see a throttle coming rather than hit it
- Works anonymously on public repositories; a token raises the limit and opens private ones
- Self-hosted GitHub Enterprise and GitLab are detected from the hostname, and a host that gives
  nothing away can be named in the config
- More than one remote: `R` walks them, and every remote-backed view follows

**CI.**

- Workflow runs from GitHub Actions and pipelines from GitLab CI, for the branch you are on
- Passed, failed, running, queued, manual, cancelled and skipped, each with its own glyph and word
  as well as its own colour
- Duration per run, counting up while one is still going, and how long ago it started
- `enter` opens the jobs of the selected run — with GitLab's stages when there are stages
- Refreshes on a timer while the view is open, and says in the header when the next one is due

**Pull requests.**

- Open pull requests from GitHub and merge requests from GitLab, in one list that says what each
  provider actually reports rather than inventing the rest
- Draft, open, merged and closed, each with its own glyph and word
- `enter` opens the branches, author, labels, reviewers and URL — a disclosure, not a request:
  everything it shows arrived with the list, so scrolling with it open costs nothing
- Mergeable, conflicting and blocked where GitLab says so; GitHub's list endpoint does not report
  mergeability, so that column simply is not drawn rather than guessed at
- The one for the branch you are standing on is marked and sorted to the top

The refresh loop is built around the rate limit rather than a stopwatch. It only runs while the CI
view is on screen, only when there is a token — sixty anonymous requests an hour does not survive a
twenty-second poll — and it stops on its own below a fifth of the remaining budget. Every one of
those cases says why in the header instead of quietly going still. Jobs are read when you ask for
them rather than for every row, so scrolling a list of runs costs nothing.

The fetch runs on a worker thread. The dashboard keeps drawing while it is in flight, and quitting
mid-request does not wait for it. Every failure has its own screen: a rejected token says which
variable or file it came from, a 404 while anonymous points out that private repositories need
one, and no network at all is reported as the ordinary thing it is.

The layout adapts: stat cards stack two-by-two below 84 columns, the heatmap yields to the log
on terminals shorter than 30 rows, and the remote view folds its count tiles into a single line
when there is no room for four.

## Push and pull

`f` fetches, `p` pulls, `P` pushes. All three run on a worker thread with a progress bar, and
`esc` gives up on one mid-flight.

Pull fast-forwards or refuses. A branch that has genuinely diverged needs a merge commit or a
rebase, and doing either implicitly behind a key called "pull" is how a tool loses work that
exists nowhere else — so gittop fetches, says what it found, and leaves the decision alone. A
fast-forward that would write over an uncommitted change stops and names the file in the way.

Push never forces, and there is no flag to make it. It asks before it runs, because it is the only
thing here that changes something other people can see. A branch with no upstream gets one, the
same as `git push -u`.

Over https, a token from the environment or the config file is the password. Over ssh, gittop runs
the `ssh` you already have — so your `~/.ssh/config`, your agent, your keys and your known_hosts
all apply, and a remote that works from the shell works here with nothing else to set up.

## Themes

Seven built in: `default`, `catppuccin`, `gruvbox`, `nord`, `tokyo-night`, `dracula`, and
`daylight`, which is a light theme designed as one rather than a dark theme inverted. `t` cycles
them; `theme.name` in the config picks one to start with.

Palettes are written in sixteen roles, so a theme of your own is those same roles under
`[theme.colors]` — anything you leave out keeps the value it had.

Colour degrades on the way to the screen rather than in the panels: truecolor when `COLORTERM`
says so, the 256-colour cube when `TERM` does, the base sixteen otherwise, and none at all under
`NO_COLOR`. Every status in gittop is a glyph and a word as well as a colour, so the monochrome
case is legible rather than merely supported.

## Keys and the mouse

Every key is rebindable under `[keys]` in the config, by action name rather than by position, and
the footer hints and the help overlay read the bindings in force rather than a list of what they
used to be.

The mouse works too: the wheel scrolls, a click selects a row or switches to a tab, and clicking
the row already under the cursor opens it.

## Custom layouts

`[layout]` decides which tabs exist and in what order:

```toml
[layout]
views = "status diff history branches stashes"
start_view = "status"
compact = false
```

Any view left out is simply not there — a reasonable thing to want on a repository with no remote
worth watching. The digit keys are positional, so `2` reaches whatever you put second and the tab
prints the same number; there is no way for a tab to advertise a key that goes somewhere else.
`compact` forces the narrow layout at any width, which gittop otherwise switches to under 84
columns on its own.

## Tokens

Public repositories need no token. GitHub allows 60 anonymous requests an hour; a token raises
that to 5000 and lets gittop read private repositories.

The environment is checked first, so a shell or a CI job never has to write a secret to disk:

```bash
export GITHUB_TOKEN=ghp_...      # or GH_TOKEN, or GITTOP_TOKEN for either provider
export GITLAB_TOKEN=glpat-...    # or CI_JOB_TOKEN
```

Otherwise gittop reads `~/.config/gittop/config.toml`, which it creates `0600`:

```bash
gittop --init-config    # writes a commented starter
gittop --config-path    # prints where it looks
```

A token is never printed back to you, masked or otherwise. The remote panel names the variable or
the file it came from and nothing else.

## Planned

**Locally, offline:**

- Staging a hunk rather than a whole file, and a word-level highlight inside a changed line
- Interactive rebase proper: reword, squash, drop, reorder — `B` today is `rebase @{upstream}`
- Stash messages, and stashing only what is staged

**From GitHub and GitLab:**

- More than one page of CI and pull request history
- Merge, so a pull that is not a fast-forward has a second option besides rebasing
- Review state and check status on a pull request row

## Current status

Seven phases of eight are done. [`progress.md`](progress.md) holds the full plan: stack decisions,
the source layout, per-task checkboxes, known risks, and a work log.

| Phase | Scope | State |
|---|---|---|
| 0 | CMake, FTXUI window, libgit2 linked, repo detection | Done |
| 1 | Local status dashboard, staging, discard, commit | Done |
| 2 | History, commit graph, activity heatmap, branches | Done |
| 3 | Config, tokens, provider detection, async HTTP | Done |
| 4 | Pipelines and CI panels | Done |
| 5 | Pull requests, push/pull, themes, mouse | Done |
| 6 | Diff viewer, stash, rebase helpers, search, layouts | Done |
| 7 | Visual design pass | Next, partly landed early |

## Keys

| Key | Action |
|---|---|
| `1` … `9` | Jump to a tab by its number |
| `tab` | Cycle through the views |
| `j` `k` or arrows | Move the selection |
| `g` `G` | First / last, or oldest / newest on the graph |
| `ctrl-u` `ctrl-d` | Move a screen at a time |
| `/` | Filter the list on screen; `esc` clears it |
| `enter` | Diff a file or a commit, jobs of a CI run, details of a pull request |
| `h` `l` | Pan the graph through time |
| `d` `w` `m` | Graph bucket: day, week, month |
| `space` | Stage or unstage the selection |
| `s` `u` | Stage / unstage explicitly |
| `a` | Stage everything |
| `d` | Discard the selection, after a confirm |
| `c` | Write a commit |
| `s` | Diff view: swap between unstaged and staged |
| `[` `]` | Diff view: previous / next file |
| `S` | Stash the whole working tree |
| `a` `p` `d` | Stash view: apply / pop / drop, the last two after a confirm |
| `B` | Rebase this branch onto its upstream, after a confirm |
| `o` | Continue or abort a rebase, merge or cherry-pick |
| `f` `p` `P` | Fetch / pull / push |
| `R` | Switch to the next remote |
| `t` | Next theme |
| `r` | Re-read the repository, or re-fetch on the remote views |
| `?` | Help |
| `q` | Quit |

Keys that appear twice are scoped to a view: `s` stages on the status view and swaps sides on the
diff view, `d` discards, changes the graph bucket, and drops a stash. Which one a key means is a
fact about where you are, not something the config decides.

## Built with

| | |
|---|---|
| Language | C++20 |
| TUI | [FTXUI](https://github.com/ArthurSonzogni/FTXUI) |
| Git access | [libgit2](https://libgit2.org/) |
| HTTP | [libcurl](https://curl.se/libcurl/) |
| JSON | [nlohmann/json](https://github.com/nlohmann/json) |
| Build | CMake with FetchContent |

## Building it

Needs a C++20 compiler, CMake 3.24 or newer, zlib, and libcurl. FTXUI, libgit2, and nlohmann/json
are fetched and built by CMake.

```bash
sudo apt install libcurl4-openssl-dev     # or libcurl-devel, or curl on Homebrew

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/gittop            # reads the repository containing the current directory
./build/gittop /some/repo # or one you name
```

The first configure downloads the fetched dependencies, so it takes a few minutes. After that,
builds are quick.

libcurl is the one dependency taken from the system rather than built here, because the
distribution's copy comes configured for the platform's CA bundle and TLS backend. A vendored one
would have to be told where the machine keeps its certificates before it could verify a request.

libgit2's transports are on, and neither costs a build dependency. HTTPS uses libgit2's
`OpenSSL-Dynamic` backend, which loads libssl at run time rather than linking it, so no OpenSSL
headers are needed to build and the binary is not tied to the version it compiled against. SSH
runs the system `ssh` instead of libssh2 — cheaper to build, and better behaved, since it inherits
your own ssh configuration.

## License

GPL-3.0. See [LICENSE](LICENSE).
