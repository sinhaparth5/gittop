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
> There is no code in this repository yet. The stack is decided and the roadmap is written, but
> nothing builds and nothing runs. If you cloned this expecting a binary, come back later.

## What it will show

**Locally, offline:**

- Working tree status: staged, unstaged, untracked, and conflicted counts with fill bars
- A file list you can move through to stage, unstage, or discard
- Commit from inside the dashboard
- Commit history as a lane graph drawn in box-drawing characters
- A commit activity heatmap over the last 30 or 90 days
- Branches with ahead/behind counts against their upstream

**From GitHub and GitLab:**

- Workflow runs (GitHub Actions) and pipelines (GitLab CI) for the current branch
- Live status per job: passing, failing, running, queued, cancelled
- Open pull requests and merge requests
- Push and pull with progress, plus remote tracking state
- Remaining API rate limit, so you can see when you are about to get throttled

Every network call happens off the UI thread. A slow API response slows down one panel, not the
whole dashboard.

## Current status

Phase 0 of seven, and Phase 0 has not started. [`progress.md`](progress.md) holds the full plan:
locked stack decisions, the target source layout, per-task checkboxes for each phase, known risks,
and a work log.

| Phase | Scope | State |
|---|---|---|
| 0 | CMake, FTXUI window, libgit2 linked, repo detection | Not started |
| 1 | Local status dashboard, staging, commit | Not started |
| 2 | History, commit graph, activity heatmap, branches | Not started |
| 3 | Config, tokens, provider detection, async HTTP | Not started |
| 4 | Pipelines and CI panels | Not started |
| 5 | Pull requests, push/pull, themes, mouse | Not started |
| 6 | Diff viewer, stash, rebase helpers, search | Not started |
| 7 | Visual design pass | Not started |

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

Not yet possible. Once `CMakeLists.txt` lands in Phase 0, the build is the CMake standard:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/gittop
```

## License

GPL-3.0. See [LICENSE](LICENSE).
