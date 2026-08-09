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
> The local half runs today. Remote pipelines, pull requests, history graphs, and themes are
> not built yet. See [Current status](#current-status) for what exists and what does not.

## What works today

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

## Planned

**Locally, offline:**

- Commit history as a lane graph drawn in box-drawing characters
- A commit activity heatmap over the last 30 or 90 days
- Branches with ahead/behind counts against their upstream
- Diff viewer, stash management, rebase helpers

**From GitHub and GitLab:**

- Workflow runs (GitHub Actions) and pipelines (GitLab CI) for the current branch
- Live status per job: passing, failing, running, queued, cancelled
- Open pull requests and merge requests
- Push and pull with progress, plus remote tracking state
- Remaining API rate limit, so you can see when you are about to get throttled

Every network call will happen off the UI thread. A slow API response should slow down one panel,
not the whole dashboard.

## Current status

Two phases of eight are done. [`progress.md`](progress.md) holds the full plan: stack decisions,
the source layout, per-task checkboxes, known risks, and a work log.

| Phase | Scope | State |
|---|---|---|
| 0 | CMake, FTXUI window, libgit2 linked, repo detection | Done |
| 1 | Local status dashboard, staging, discard, commit | Done |
| 2 | History, commit graph, activity heatmap, branches | Next |
| 3 | Config, tokens, provider detection, async HTTP | Planned |
| 4 | Pipelines and CI panels | Planned |
| 5 | Pull requests, push/pull, themes, mouse | Planned |
| 6 | Diff viewer, stash, rebase helpers, search | Planned |
| 7 | Visual design pass | Partly landed early |

## Keys

| Key | Action |
|---|---|
| `j` `k` or arrows | Move the selection |
| `g` `G` | First / last |
| `space` | Stage or unstage the selection |
| `s` `u` | Stage / unstage explicitly |
| `a` | Stage everything |
| `d` | Discard the selection, after a confirm |
| `c` | Write a commit |
| `r` | Re-read the repository |
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

Needs a C++20 compiler, CMake 3.24 or newer, and zlib. FTXUI and libgit2 are fetched and built
by CMake, so there is nothing to install first.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/gittop            # reads the repository containing the current directory
./build/gittop /some/repo # or one you name
```

The first configure downloads both dependencies, so it takes a few minutes. After that, builds
are quick. libgit2 is compiled with its HTTPS and SSH transports off, since nothing here touches
the network yet; Phase 5 turns them back on.

## License

GPL-3.0. See [LICENSE](LICENSE).
