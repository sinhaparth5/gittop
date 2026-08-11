# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

**Read `progress.md` first.** It holds the locked stack decisions, the phased roadmap with
per-task checkboxes, known risks, and a work log. It is the source of truth for project state;
this file describes the architecture only.

## What this is

`gittop` is a btop-inspired terminal dashboard for Git: local repository state that works with no
network, plus GitHub and GitLab panels. Phases 0 through 6 are done, so there are nine views —
status, history, branches, graph, diff, stashes, remote, CI, and pull requests — plus push/pull,
stashing, rebase helpers, a `/` filter, themes, rebindable keys, a configurable tab set and the
mouse.

Trust `progress.md` for phase state, not the git log: the commit messages are off by one and
misspell "phase", so `phrase 4 finished` is the commit that landed Phase 3.

## Build and run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j
./build/gittop [path]
```

Needs system libcurl (`libcurl4-openssl-dev`) and zlib — and nothing else. FTXUI v7.0.3, libgit2
v1.9.6, and nlohmann/json v3.12.0 come from FetchContent, so the first configure in a fresh tree
spends a few minutes downloading and building them — budget for it rather than assuming the build
hung. `-Wall -Wextra -Wpedantic` is set on the target, so warnings show up without asking.

libgit2 is built with `USE_HTTPS=OpenSSL-Dynamic` and `USE_SSH=exec`, chosen so that turning the
transports on in Phase 5 added no build dependency: the first dlopen()s libssl at run time and
needs no OpenSSL headers, and the second runs the system `ssh` rather than linking libssh2. Do not
"fix" these to the conventional `ON` without a reason — `ON` would mean two new dev packages and,
for SSH, worse behaviour, since the exec transport is what makes gittop honour the user's own
`~/.ssh/config`, agent and known_hosts.

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

Transfers need three different fakes, because each one reaches a different code path:

- **A local-path remote** (`git init --bare` plus a clone) covers fetch, fast-forward pull,
  divergence, a blocked checkout and `push -u`. It does *not* cover server rejection: libgit2's
  local transport copies objects directly and never runs receive-pack, so hooks never fire.
- **`git daemon --listen=127.0.0.1 --enable=receive-pack`** over a bare repo with a `pre-receive`
  hook that exits non-zero is the only way to exercise the real smart protocol offline, and the
  only way to reach the `push_update_reference` rejection path.
- **A socket that accepts and then never writes** (six lines of Python) is how the transport
  timeouts get tested. Without them gittop cannot be quit at all while one is in flight.

Mouse input is drivable too: SGR sequences straight into stdin — `\033[<0;X;YM` and the matching
`m` for a click, button 64 and 65 for the wheel. Terminal `y=2` is the tab row.

Measuring how long the app takes to *exit* needs care: in `keys | script -qec ...` the shell waits
for both halves, so a trailing `sleep` in the producer is what you end up timing rather than the
program. Drive it through a fifo on fd 3 and poll `kill -0` on the pid instead.

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
thing keeping the Phase 7 visual pass a restyling job rather than a rewrite of every panel. It is
also what made seven themes and the 256/16/monochrome fallback a change to one file: palettes are
written in sixteen roles and composed into the thirty semantic tokens, and every color funnels
through `ToColor`, which is where quantization happens. Add a token by adding it to `Compose`, not
by reaching for a literal at the call site.

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

Since Phase 5 that handler does not compare against `Event` literals: it asks `ui::Keymap` for an
`Action` and calls `App::Perform`. A new key is a row in the table in `keymap.cpp` plus a case in
`Perform`, and it becomes rebindable for free. `Scope` is how the graph takes `d`, `g` and `G`
while it is on screen — `Lookup` tries the view's scope before the global one — and it is a
property of the action rather than something the config can set, because where an action applies
is a fact about what it does.

**Reads are pure and return snapshots.** `ReadStatus`, `ReadHistory`, `FetchRepoInfo`,
`FetchPipelines`, `FetchJobs` and `FetchPulls` allocate their own results and touch no UI state,
which is what let the remote fetch move to a worker thread without changing anything else. One
thread must own a `Repository` at a time — libgit2 objects are not safe for concurrent use, which
is why `git/transfer.cpp` opens its own `git_repository` from a path rather than borrowing App's.

## Adding a view

A view is not one file, but Phase 5 removed three of the places it used to be. `ui::AllViews()` in
`panels.cpp` is now the single ordered list, and the tab bar, the `tab`/`backtab` cycle and the
tab hit-boxes all read it — adding an entry there gets all three. What is still manual: the
`ui::View` enum, `TabLabel`, a `view_*` action in `keymap.cpp` with its digit, `ActiveSelection`
and `ActiveCount` for the selection state, `CurrentScope` if the view claims keys of its own,
`SetView` if it loads anything, the render tree, `Footer`'s per-view hints, and the help overlay.
Miss one and the view exists but cannot be reached, or scrolls the wrong list.

## Transfers

`git/transfer.cpp` is the only file that reaches the network with libgit2. Three rules it exists to
keep:

- **Its own repository handle.** Opened from a path on the worker thread, never App's `repo_`.
- **The result comes back through the fetcher; progress does not.** A transfer has to be visible
  while it is still running, so `ProgressSink` is a mutex around one struct, held by `shared_ptr`
  so the worker never reaches into App.
- **`git_remote_push` returns zero for a push the server refused.** The refusal arrives only
  through the `push_update_reference` callback. Anything that calls push and does not read
  `ctx.rejections` will report success for a rejected non-fast-forward.

Cancellation is only checked from libgit2's progress callbacks, so a server that accepts and then
says nothing never fires one. `git::Library` sets `GIT_OPT_SET_SERVER_CONNECT_TIMEOUT` and
`GIT_OPT_SET_SERVER_TIMEOUT`; they are the only thing bounding how long quitting can block on a
stalled socket. Any future long-running libgit2 call needs the same question asked of it.

## ssh authentication and `git/askpass.cpp`

**libgit2's credential callback is never consulted for an ssh remote on this build.** `USE_SSH=exec`
means libgit2 builds an `ssh` command line and execs it; every credential decision happens inside
that child. The `GIT_CREDENTIAL_SSH_KEY` branch in `transfer.cpp`'s `CredentialCb` is dead code here
and only exists for a libssh2 build. Adding a passphrase prompt there does nothing — the question
is not being asked there. This is not obvious and it is worth an hour of somebody's time.

The one channel into that child is `SSH_ASKPASS`, and it is the whole of `git/askpass.cpp`. gittop
points `SSH_ASKPASS` at its own binary and serves the passphrase to the copy of itself that ssh
spawns. Four things that shape the file:

- **`SSH_ASKPASS_REQUIRE=force` is load-bearing.** Without it ssh prefers the terminal, and the
  terminal is the one place gittop cannot let it have — the TUI is holding it in raw mode on the
  alternate screen. That is the whole reason the passphrase never reached the user before.
- **The helper is chosen by the environment, not a flag**, and the check is the first thing in
  `main` — ssh puts *its own prompt* in `argv[1]`, so the argument parser would otherwise try to
  open a repository called `Enter passphrase for key '...':`.
- **The passphrase travels over a unix socket**, not the environment, argv, or a file.
  `/proc/<pid>/environ` and `/cmdline` are unprivileged reads for the same user, and a file would
  put a private key's passphrase on disk. The socket sits in a 0700 `mkdtemp` directory and is
  unlinked with the transfer. The listener needs its own thread because ssh asks while the transfer
  worker is already blocked in libgit2 waiting for that same child.
- **`InstallAskpassEnv` writes to gittop's own environment**, because the exec transport gives no
  way to set the child's. That makes it process-global, so it is called from the UI thread with no
  transfer in flight, and cleared in `CollectTransfer`.

`NeedsPassphrase` requires all three of: an ssh URL, no agent holding an identity, and a default
`~/.ssh` key that is actually encrypted. Prompting when the answer is not needed is its own bug —
an unencrypted key with no agent authenticates perfectly well, and a box asking for a passphrase is
indistinguishable from the thing users are told never to type into. It can still be wrong in both
directions, because which key ssh picks depends on `~/.ssh/config` and gittop does not parse it;
neither error is expensive. `AskpassServer::served()` is what makes a wrong passphrase distinct
from an ordinary failure — and a success where nothing ever asked means the passphrase was not what
authenticated, so it is dropped rather than held for a session that does not need it.

Pull fast-forwards or refuses, push never forces, and push is the only operation that asks first —
it is the only one that changes something other people can see. Keep it that way; the
destructive-operations rule in `progress.md` is what these implement.

## The mouse and hit-testing

FTXUI cannot be asked where a dom node ended up: a node's box is only filled in during layout, and
the lists scroll inside a `yframe`, so a row's screen position has nothing to do with its index.
Every list therefore takes an optional `std::vector<ftxui::Box>*` and attaches `reflect()` to each
row; App matches a click against them on the next event, which works because FTXUI renders before
it reads input. A hit is also checked against the panel's own box, because a row scrolled out of
its frame still gets a box and it can land somewhere else on screen. Any new list wanting clicks
follows the same shape rather than computing rows from a y offset.

## Lazy reads and cache invalidation

Only the status snapshot is read at startup. History is read on the first switch to a view that
needs it (`EnsureHistory`) and cached; the remote, the CI runs and the pull requests are fetched on
the first switch to their tabs (`EnsureRemote`, `EnsurePipelines`, `EnsurePulls`), on worker
threads. Switching remotes with `R` drops all three, because none of them describes the new one. Opening a repository must
never pay for a revwalk or an HTTP round-trip nobody asked to see. Committing invalidates the
history cache and `r` forces a reload of whichever view is active. Anything new that costs real
time belongs on the same terms.

A request is also a cost the *user* did not ask for, which is why job drill-down is on `enter`
rather than on the cursor: following the selection would put a request on every keystroke. The
pull request detail pane is deliberately not the same — it costs nothing, since everything it shows
came back with the list, so moving the cursor keeps it open while moving off a CI run closes its
jobs. That asymmetry is the point, not an inconsistency. The CI
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

`[theme]`, `[theme.colors]` and `[keys]` are read in `App::ApplyConfig`, which runs in the
constructor so nothing is ever drawn in a palette the user replaced. It scans by key prefix rather
than by a list of known names, which is what makes `theme.colors.*` and `keys.*` open sets. Every
rejection is reported by name — an unknown role, an unparseable colour, an unknown action, a key
spec gittop cannot read — and since the toast is one line, they are counted and the first is
shown. Silently ignoring a config line is the one thing not to do here.

## Async

`git::Library` and `remote::HttpLibrary` are process-wide and their inits are not thread-safe, so
`main` constructs both before anything can spawn a worker. Keep it that way.

`remote::Fetcher<Result>` is a header-only template that runs one task off the UI thread and hands
the result back on it. App holds one per concern — repo, pipelines, jobs, pulls, transfer — rather
than queueing on a shared instance, so a refresh that fires on a timer can never sit in front of a
fetch the user just asked for by pressing `r`. Each posts its own named `Event::Special` and each
result is picked up by its `Collect*` inside the root `CatchEvent`, on the UI thread.

`Cancel()` and `Shutdown()` are not the same thing and the difference matters: `Shutdown()` is
teardown and throws the result away, `Cancel()` is a user pressing esc and still wants the
"cancelled" result delivered. That is what `discard_result_` separates.

Every notifier captures the `ScreenInteractive` **by reference**, so all three fetchers and
`remote::Ticker` must be shut down before that screen is destroyed; `App::Run` does it immediately
after `Loop()` returns. Any future worker needs the same treatment.

`remote::Ticker` exists because an idle dashboard requests no frames — that is the point of the
`RequestAnimationFrame` arrangement in `App::Tick` — so nothing would ever notice a refresh
interval elapsing. Asking for animation frames instead would mean repainting at sixty hertz for
twenty seconds to watch a clock. It posts one event a second, and only while the CI view is open.

## Secrets

The token lives in `remote::Token` and nothing under `ui/` takes one. The one exception is
`git::Credentials`, which libgit2 requires as a plain username/password pair for an https push;
it is built per transfer, lives inside the task, and is never stored on App. The remote panel names the
variable or the file a token came from and never the value, masked or otherwise. Config files are
written `0600` and their directory `0700`. A remote URL with embedded credentials gets its
userinfo replaced before it reaches the screen (`SafeUrl` in `ui/remote_panel.cpp`). App records
where a token came from but never the value: `DiscoverRemotes` keeps `token_source` and
`token_origin`, and each fetch resolves the secret again into a `Token` that lives no longer than
the task holding it.

The ssh key passphrase is the second exception and is handled the same way. `App::ssh_passphrase_`
holds it for the process so a session is asked once, and it is cleared the moment it is shown to be
wrong or shown to be unnecessary. `ui::PassphrasePane` takes the *already-rendered* `Element`
rather than the string, so the rule that nothing under `ui/` handles a secret survives a panel
whose entire job is collecting one — and the `Input` is in password mode, so the element carries
asterisks and not the passphrase. It is never written to the config, the log, or the screen.

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
