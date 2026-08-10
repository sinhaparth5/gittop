# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

**Read `progress.md` first.** It holds the locked stack decisions, the phased roadmap with
per-task checkboxes, known risks, and a work log. It is the source of truth for project state;
this file describes the architecture only.

## What this is

`gittop` is a btop-inspired terminal dashboard for Git: local repository state that works with no
network, plus GitHub and GitLab panels. Phases 0 through 4 are done, so there are six views —
status, history, branches, graph, remote, and CI.

Trust `progress.md` for phase state, not the git log: the commit messages are off by one and
misspell "phase", so `phrase 4 finished` is the commit that landed Phase 3.

## Build and run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j
./build/gittop [path]
```

Needs system libcurl (`libcurl4-openssl-dev`) and zlib. FTXUI v7.0.3, libgit2 v1.9.6, and
nlohmann/json v3.12.0 come from FetchContent, so the first configure in a fresh tree spends a few
minutes downloading and building them — budget for it rather than assuming the build hung.
`-Wall -Wextra -Wpedantic` is set on the target, so warnings show up without asking.

Beyond `[path]`, the binary takes `--config <file>`, `--config-path`, `--init-config`,
`--version`, and `-h`. The first three exit before any TUI or libgit2 work, which makes
`--config-path` and `--init-config` the cheapest way to exercise the config layer.

There is no test target yet; verification so far has
been done by driving the TUI under `script -qec "stty rows N cols M; timeout -s KILL 20 ./build/gittop"`
with piped keystrokes, then stripping ANSI with `sed -r 's/\x1B\[[0-9;?]*[A-Za-z]//g'`. Note that
those captures are binary as far as grep is concerned, so `grep -a` is needed on them. Sleep
between the piped keys, or the app quits before a fetch has landed, and read the *last* frame —
a capture holds every frame drawn, so `head` shows the startup screen.

Remote work is verified against a throwaway stub API rather than a live one: a Python
`http.server` that answers both providers' shapes, a `git init`'d repo whose `origin` points at a
made-up host, and a config naming that host's provider and `api` base. That combination reaches
every branch — self-hosted detection, both auth headers, rate-limit headers, 4xx and 5xx, and a
port with nothing behind it for the offline path — without spending a real budget. A stub that
logs auth header *names* proves the scheme without printing a token.

## Layout

```
src/
├── main.cpp        argument parsing, library init, repository discovery
├── app.cpp/.hpp    all application state, all key routing, the render tree
├── model/          provider-neutral types: status, history, remote, pipeline. Headers only
├── git/            libgit2 wrapper (repository.cpp) and lane assignment (graph.cpp)
├── config/         a hand-written strict-TOML-subset reader and writer (no parser dependency)
├── remote/         provider detection, tokens, HTTP, the shared api.cpp, one file per endpoint
│                   family (client = repo, pipelines = CI), plus the worker and the ticker
└── ui/             one file per panel group, plus theme.cpp and widgets.cpp
```

## Four rules that keep this codebase working

**`ui/` names semantic roles, never colors.** Everything comes from `ui::theme()`. A raw
`ftxui::Color` literal anywhere under `src/ui/` outside `theme.cpp` is a bug, and it is the only
thing keeping the Phase 7 visual pass a restyling job rather than a rewrite of every panel.

**`model/` types are provider-neutral.** GitHub says `stargazers_count` and GitLab says
`star_count`; both become `RepoInfo::stars` in `remote/client.cpp`. If a file under `ui/` ever
needs to know which provider replied, the abstraction has leaked. `model::RunStatus` is the hard
case and the one to copy: GitHub splits a run's state across `status` and `conclusion` while
GitLab sends one field with eleven values, and both foldings live in `remote/pipelines.cpp` alone.
What a provider does not send stays empty rather than defaulted — GitLab has no watchers and no
commit title on a pipeline, GitHub has no stages — so a panel can omit a field instead of printing
a zero that reads as a fact about the repository.

**All key routing lives in one `CatchEvent` on the root component in `app.cpp`.** This is not
style. `Container::Stacked` and `Container::Tab` only deliver events to a *focused* child, and the
only focusable component in the tree is the commit `Input`. Handlers attached to individual panes
silently never run. This has already caused two bugs.

**Reads are pure and return snapshots.** `ReadStatus`, `ReadHistory`, `FetchRepoInfo`,
`FetchPipelines` and `FetchJobs` allocate their own results and touch no UI state, which is what
let the remote fetch move to a worker thread without changing anything else. One thread must own a
`Repository` at a time — libgit2 objects are not safe for concurrent use.

## Adding a view

A view is not one file. `ui::View` in `ui/panels.hpp` is the enum, and everything else lives in
`app.cpp`: `TabBar` needs the label, the root `CatchEvent` needs the digit key and a slot in the
`tab` cycle, `ActiveSelection` and `ActiveCount` need the view's selection state, the render tree
needs the panel, and `Footer` needs its per-view key hints. Miss one and the view exists but
cannot be reached, or scrolls the wrong list. The CI view on tab 6 walked all six, and the help
overlay in `panels.cpp` is a seventh place worth remembering.

## Lazy reads and cache invalidation

Only the status snapshot is read at startup. History is read on the first switch to a view that
needs it (`EnsureHistory`) and cached; the remote and the CI runs are fetched on the first switch
to their tabs (`EnsureRemote`, `EnsurePipelines`), on worker threads. Opening a repository must
never pay for a revwalk or an HTTP round-trip nobody asked to see. Committing invalidates the
history cache and `r` forces a reload of whichever view is active. Anything new that costs real
time belongs on the same terms.

A request is also a cost the *user* did not ask for, which is why job drill-down is on `enter`
rather than on the cursor: following the selection would put a request on every keystroke. The CI
poll loop runs only while its view is on screen, only when authenticated, and stops below a fifth
of the remaining budget — and says which of those it is doing in the panel header, because a
dashboard that has silently stopped updating looks exactly like one where nothing is happening.

## Config

Keys are flat dotted paths with quotes stripped, so `hosts."gitlab.internal".token` is looked up as
`hosts.gitlab.internal.token` and reached through `HostValue(host, field)`. Per-host settings are
table keys rather than code, which is what lets a self-hosted instance be configured without a
patch. The reader accepts comments, table headers, and `key = value` for strings, ints, and bools —
that is the entire language. It is a strict subset rather than a lookalike, so a real TOML parser
can be dropped in later without migrating anyone's file.

## Async

`git::Library` and `remote::HttpLibrary` are process-wide and their inits are not thread-safe, so
`main` constructs both before anything can spawn a worker. Keep it that way.

`remote::Fetcher<Result>` is a header-only template that runs one task off the UI thread and hands
the result back on it. App holds one per concern — repo, pipelines, jobs — rather than queueing on
a shared instance, so a refresh that fires on a timer can never sit in front of a fetch the user
just asked for by pressing `r`. Each posts its own named `Event::Special` and each result is picked
up by its `Collect*` inside the root `CatchEvent`, on the UI thread.

Every notifier captures the `ScreenInteractive` **by reference**, so all three fetchers and
`remote::Ticker` must be shut down before that screen is destroyed; `App::Run` does it immediately
after `Loop()` returns. Any future worker needs the same treatment.

`remote::Ticker` exists because an idle dashboard requests no frames — that is the point of the
`RequestAnimationFrame` arrangement in `App::Tick` — so nothing would ever notice a refresh
interval elapsing. Asking for animation frames instead would mean repainting at sixty hertz for
twenty seconds to watch a clock. It posts one event a second, and only while the CI view is open.

## Secrets

The token lives in `remote::Token` and nothing under `ui/` takes one. The remote panel names the
variable or the file a token came from and never the value, masked or otherwise. Config files are
written `0600` and their directory `0700`. A remote URL with embedded credentials gets its
userinfo replaced before it reaches the screen (`SafeUrl` in `ui/remote_panel.cpp`). App records
where a token came from but never the value: `DiscoverRemotes` keeps `token_source` and
`token_origin`, and each fetch resolves the secret again into a `Token` that lives no longer than
the task holding it.

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
