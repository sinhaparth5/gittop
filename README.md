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
> The local half runs today, and so does reading repository state and CI from GitHub and GitLab.
> Pull requests, push/pull, and themes are not built yet. See
> [Current status](#current-status) for what exists and what does not.

## What works today

Six views, switched with `1` through `6` or cycled with `tab`.

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

**Remote.**

- Reads the repository from GitHub or GitLab: description, default branch, visibility, last push
- Stars, forks, open issues, and watchers
- Remaining API budget as a bar, so you can see a throttle coming rather than hit it
- Works anonymously on public repositories; a token raises the limit and opens private ones
- Self-hosted GitHub Enterprise and GitLab are detected from the hostname, and a host that gives
  nothing away can be named in the config

**CI.**

- Workflow runs from GitHub Actions and pipelines from GitLab CI, for the branch you are on
- Passed, failed, running, queued, manual, cancelled and skipped, each with its own glyph and word
  as well as its own colour
- Duration per run, counting up while one is still going, and how long ago it started
- `enter` opens the jobs of the selected run — with GitLab's stages when there are stages
- Refreshes on a timer while the view is open, and says in the header when the next one is due

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

- Diff viewer, stash management, rebase helpers, search

**From GitHub and GitLab:**

- Open pull requests and merge requests
- Push and pull with progress, plus remote tracking state
- More than one page of CI history, and more than one remote

## Current status

Five phases of eight are done. [`progress.md`](progress.md) holds the full plan: stack decisions,
the source layout, per-task checkboxes, known risks, and a work log.

| Phase | Scope | State |
|---|---|---|
| 0 | CMake, FTXUI window, libgit2 linked, repo detection | Done |
| 1 | Local status dashboard, staging, discard, commit | Done |
| 2 | History, commit graph, activity heatmap, branches | Done |
| 3 | Config, tokens, provider detection, async HTTP | Done |
| 4 | Pipelines and CI panels | Done |
| 5 | Pull requests, push/pull, themes, mouse | Next |
| 6 | Diff viewer, stash, rebase helpers, search | Planned |
| 7 | Visual design pass | Partly landed early |

## Keys

| Key | Action |
|---|---|
| `1` … `6` | Status / History / Branches / Graph / Remote / CI |
| `tab` | Cycle through the views |
| `j` `k` or arrows | Move the selection |
| `g` `G` | First / last, or oldest / newest on the graph |
| `h` `l` | Pan the graph through time |
| `d` `w` `m` | Graph bucket: day, week, month |
| `space` | Stage or unstage the selection |
| `s` `u` | Stage / unstage explicitly |
| `a` | Stage everything |
| `d` | Discard the selection, after a confirm |
| `c` | Write a commit |
| `enter` | Jobs of the selected CI run |
| `r` | Re-read the repository, or re-fetch on the remote and CI views |
| `?` | Help |
| `q` | Quit |

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

libgit2 is compiled with its own HTTPS and SSH transports off: gittop talks to the REST APIs
through libcurl, and nothing yet asks libgit2 to reach the network. Phase 5 adds push and pull,
which is where they come back on.

## License

GPL-3.0. See [LICENSE](LICENSE).
