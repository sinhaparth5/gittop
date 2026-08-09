# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

**Read `progress.md` first.** It holds the locked stack decisions, the phased roadmap with
per-task checkboxes, known risks, and a work log. It is the source of truth for project state;
this file describes the architecture only.

## What this is

`gittop` is a btop-inspired terminal dashboard for Git: local repository state that works with no
network, plus GitHub and GitLab panels. Phases 0 through 3 are done, so there are five views —
status, history, branches, graph, and remote.

## Build and run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j
./build/gittop [path]
```

Needs system libcurl (`libcurl4-openssl-dev`) and zlib. FTXUI v7.0.3, libgit2 v1.9.6, and
nlohmann/json v3.12.0 come from FetchContent. There is no test target yet; verification so far has
been done by driving the TUI under `script -qec "stty rows N cols M; timeout -s KILL 20 ./build/gittop"`
with piped keystrokes, then stripping ANSI with `sed -r 's/\x1B\[[0-9;?]*[A-Za-z]//g'`. Note that
those captures are binary as far as grep is concerned, so `grep -a` is needed on them.

## Layout

```
src/
├── main.cpp        argument parsing, library init, repository discovery
├── app.cpp/.hpp    all application state, all key routing, the render tree
├── model/          provider-neutral types: status, history, remote. Headers only
├── git/            libgit2 wrapper (repository.cpp) and lane assignment (graph.cpp)
├── config/         the config file reader and writer
├── remote/         provider detection, token resolution, HTTP, API clients, the fetch thread
└── ui/             one file per panel group, plus theme.cpp and widgets.cpp
```

## Four rules that keep this codebase working

**`ui/` names semantic roles, never colors.** Everything comes from `ui::theme()`. A raw
`ftxui::Color` literal anywhere under `src/ui/` outside `theme.cpp` is a bug, and it is the only
thing keeping the Phase 7 visual pass a restyling job rather than a rewrite of every panel.

**`model/` types are provider-neutral.** GitHub says `stargazers_count` and GitLab says
`star_count`; both become `RepoInfo::stars` in `remote/client.cpp`. If a file under `ui/` ever
needs to know which provider replied, the abstraction has leaked. The same will hold for Phase 4,
where workflow runs and pipelines have genuinely different shapes and both have to land in one
`model::Pipeline`.

**All key routing lives in one `CatchEvent` on the root component in `app.cpp`.** This is not
style. `Container::Stacked` and `Container::Tab` only deliver events to a *focused* child, and the
only focusable component in the tree is the commit `Input`. Handlers attached to individual panes
silently never run. This has already caused two bugs.

**Reads are pure and return snapshots.** `ReadStatus`, `ReadHistory`, and `FetchRepoInfo` allocate
their own results and touch no UI state, which is what let the remote fetch move to a worker
thread without changing anything else. One thread must own a `Repository` at a time — libgit2
objects are not safe for concurrent use.

## Async

The fetch worker posts `Event::Special("gittop:remote-ready")` and the result is picked up by
`App::CollectFetch` inside the root `CatchEvent`, on the UI thread. The notifier captures the
`ScreenInteractive` **by reference**, so `Fetcher::Shutdown()` must run before that screen is
destroyed; `App::Run` calls it immediately after `Loop()` returns. Any future worker needs the
same treatment.

## Secrets

The token lives in `remote::Token` and nothing under `ui/` takes one. The remote panel names the
variable or the file a token came from and never the value, masked or otherwise. Config files are
written `0600` and their directory `0700`. A remote URL with embedded credentials gets its
userinfo replaced before it reaches the screen (`SafeUrl` in `ui/remote_panel.cpp`).

## FTXUI gotchas already paid for

- `canvas(fn)` looks like it auto-fits its box; it hardcodes 12×12. Size canvases from
  `screen.dimx()`.
- The canvas callback runs during Render, *after* the building function has returned. Capture
  everything by value.
- The dom cannot query its own size. `screen.dimx()` / `dimy()` are read inside the Renderer
  lambda and passed down, which is how every responsive breakpoint works.
- `flex` belongs on the panel itself, not on a `vbox` wrapped around it. Wrapping stretches the
  container and leaves the box at its content size.

## Conventions

- **License: GPL-3.0**, but sources carry no per-file header; do not start adding them.
- Two-space indent, `-Wall -Wextra -Wpedantic` clean.
- Comments explain *why*, especially where a simpler-looking approach was tried and failed. Match
  that density rather than annotating what the code already says.
- Remote: `git@github.com:sinhaparth5/gittop.git`, default branch `master`.
