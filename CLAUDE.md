# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`gittop` is a btop-inspired terminal dashboard for Git: local repository state that works with no
network, plus GitHub and GitLab panels. There are ten views — status, history, branches, graph,
diff, stashes, remote, CI, pull requests and settings — plus push/pull, stashing, rebase helpers,
sign-in, a `/` filter, eight themes, rebindable keys, a configurable tab set and the mouse. Under
all of it sit two token layers (colours and glyphs), one `Panel()` every framed panel goes through,
cell-accurate text measurement, and a motion budget.

This file is the architecture. `CHANGELOG.md` is what shipped and when — it is written per release
and is the only running account of the project's state, so a question about *when* something
arrived is answered there and not from the git log, whose early commit messages number their phases
off by one and misspell "phase" (`phrase 4 finished` landed Phase 3). Those phase numbers survive
below only as shorthand for when a rule was introduced.

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

**There is no test target and CI runs no tests.** `CMakeLists.txt` calls neither `enable_testing()`
nor `add_test()`, and `.github/workflows/release.yml` fires on a tag and only builds, packages and
smoke-tests the `.deb` — so nothing anywhere checks a behavioural change for you, and a claim that
something works has to come from having driven it. Verification so far has
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
├── model/          provider-neutral types: status, history, diff, stash, operation, remote,
│                   pipeline, pull. Headers only, so anything needing a definition is inline
├── git/            the libgit2 wrapper, split by concern. `Repository`'s methods live across
│                   repository.cpp, diff.cpp, stash.cpp and rebase.cpp — a class's methods can be
│                   defined in any translation unit, and four libgit2 callbacks for the diff have
│                   nothing to say to the revwalk. internal.hpp is the little they share.
│                   Also graph.cpp (lane assignment), transfer.cpp and askpass.cpp
├── config/         a hand-written strict-TOML-subset reader and writer (no parser dependency)
├── remote/         provider detection, tokens, HTTP, the shared api.cpp, one file per endpoint
│                   family (client = repo, pipelines = CI), plus the worker and the ticker
└── ui/             one file per panel group, plus the four files that are not panels at all:
                    theme.cpp (colour tokens), glyphs.cpp (character tokens), keymap.cpp (the
                    binding table) and widgets.cpp (`Panel`, `Truncate`). panels.cpp additionally
                    owns `AllViews()` and the tab bar, so it is where a new view starts
```

`app.cpp` is 3.5k lines and holds every piece of state and every key; `panels.cpp` and
`repository.cpp` are the next largest. Nothing here is worth opening blind — the four rules below
name the file that owns each decision, and this repository carries a CodeGraph index, so
`codegraph explore "<symbol>"` gets to the definition and its callers in one step.

## Four rules that keep this codebase working

**`ui/` names semantic roles, never colors — and since Phase 7, never characters either.** Everything comes from `ui::theme()`. A raw
`ftxui::Color` literal anywhere under `src/ui/` outside `theme.cpp` is a bug, and it is the only
thing keeping the Phase 7 visual pass a restyling job rather than a rewrite of every panel. It is
also what made seven themes and the 256/16/monochrome fallback a change to one file: palettes are
written in sixteen roles and composed into the semantic tokens, and every color funnels
through `ToColor`, which is where quantization happens. Add a token by adding it to `Compose`, not
by reaching for a literal at the call site.

The same rule now covers glyphs, for the same reason and with the same shape. Phase 6 left `"✓"` in
four files and `"○"` in five, each an independent bet about what a terminal can draw, which is the
trap the colour tokens exist to avoid. `ui/glyphs.hpp` names sixty-odd roles; `glyphs.cpp` holds
three sets (`ascii`, `unicode`, `nerd`) written with designated initializers so a new role fails to
compile in the two sets that forgot it rather than becoming a null pointer. **A raw non-ASCII
literal anywhere under `src/ui/` outside `glyphs.cpp` is a bug**, with one exception: the box
drawing FTXUI itself emits for borders and separators, which gittop does not choose.

`auto` never selects the nerd set. There is no way to ask a terminal whether its font has the
private-use icons, and guessing wrong fills the screen with tofu — so it is opt-in and nothing else
is. Ascii is chosen for a non-UTF-8 locale, `TERM=dumb` or `TERM=linux`; unicode otherwise.

**The nerd icons are written as `\uXXXX` escapes and have to stay that way.** They are private-use
code points: invisible in an editor without a patched font, dropped by anything that normalises
text, and silent when they go. Twenty-nine of the thirty-one were empty strings from Phase 7 until
2026.08.2 — the comments naming them were all still there — and three releases shipped a nerd set
that drew blanks, because noticing needs both a patched font and an opt-in nobody defaults into.
An escape is ASCII and survives every pipeline a character does not. Look code points up by the
`nf-*` name beside each entry, in `glyphnames.json` in ryanoasis/nerd-fonts.

**The brand marks are the one glyph decision that can differ from `theme.icons`.** `ui::
ProviderGlyph(provider)` is the only way to draw one — the switch used to be copied into five
panels, which is five places for a third provider to be forgotten — and `theme.logos` makes it read
the nerd set for those roles alone, whatever set everything else is using. The exception earns
itself: those marks are *logos*, and no arrangement of geometric shapes is the octocat or the
tanuki, whereas the unicode `✓` is a perfectly good check. It is still opt-in for the same tofu
reason `auto` never picks nerd, and it is ignored under `icons = "ascii"`, where the terminal has
already said it cannot carry the bytes. Everything that names a provider, a host or a remote puts
the mark in front: the remote header, the CI header, the pull header, the sign-in title, and the
settings page's remote row and REMOTES card.

**`ui::CiGlyph(provider)` is the second one, because a CI system is its own brand.** The CI header
prints the words "GitHub Actions" and used to draw the plain octocat beside them, which is the only
place gittop put one company's mark next to another product's name. Actions has a mark of its own
(`nf-dev-githubactions`) and now gets it. There is deliberately no `gitlab_ci` role to match:
GitLab CI is branded as GitLab, so `CiGlyph` delegates to `ProviderGlyph` for everything that is
not GitHub, and inventing a second tanuki-ish shape would be the only wrong answer available. Both
functions test `theme.logos` the same way rather than one calling the other's accessor — they can
appear on adjacent rows, and one drawing a logo while the other drew a hexagon is exactly the kind
of disagreement the single-source rules here exist to prevent.

**Look every nerd code point up, and check it after.** `glyphnames.json` in ryanoasis/nerd-fonts is
the authoritative list; the escapes are private-use code points, so a wrong one is invisible without
a patched font and silent with one. Validating all 34 shipped points against that file takes a
dozen lines of Python and is the only thing standing between this set and the three releases that
shipped blank glyphs.

**`model/` types are provider-neutral.** GitHub says `stargazers_count` and GitLab says
`star_count`; both become `RepoInfo::stars` in `remote/client.cpp`. If a file under `ui/` ever
needs to know which provider replied, the abstraction has leaked. `model::RunStatus` is the hard
case and the one to copy: GitHub splits a run's state across `status` and `conclusion` while
GitLab sends one field with eleven values, and both foldings live in `remote/pipelines.cpp` alone.
What a provider does not send stays empty rather than defaulted — GitLab has no watchers and no
commit title on a pipeline, GitHub has no stages — so a panel can omit a field instead of printing
a zero that reads as a fact about the repository.

**Every framed panel goes through `ui::Panel()`, and every measurement through `ui::Truncate`.**
`Panel(title, content, style)` in `widgets.cpp` is the only place a border style, a title colour or
a focus state is decided, which is what makes `[theme] border` one config line instead of seventeen
call sites and makes "which panel has the cursor" a question with a visible answer — `focused`
takes the accent border, sidecars recede, and `alarm` outranks both because an error nobody looked
at is still an error. The three frames that are not panels (the summary card row, the overlays) take
`FramedBorder()` for the same reason.

Text is measured in **cells**, never bytes or code points. `TextWidth`, `Truncate`, `Fit` and
`Rjust` wrap FTXUI's `string_width` and `Utf8ToGlyphs`; the latter returns one entry per *cell*,
with an empty string standing in for the second column of a double-width glyph, which is exactly the
invariant a truncation needs — cutting at index n is safe unless entry n is that empty half. Get
this wrong and a Japanese commit message tears the right-hand border. `size(WIDTH, EQUAL, n)` on
server text is the bug this replaced: FTXUI clips at the cell and says nothing, so a value that ran
out of room and one that happens to end there look identical.

**All key routing lives in one `CatchEvent` on the root component in `app.cpp`.** This is not
style. `Container::Stacked` and `Container::Tab` only deliver events to a *focused* child, and the
only focusable component in the tree is the commit `Input`. Handlers attached to individual panes
silently never run. This has already caused two bugs.

Since Phase 5 that handler does not compare against `Event` literals: it asks `ui::Keymap` for an
`Action` and calls `App::Perform`. A new key is a row in the table in `keymap.cpp` plus a case in
`Perform`, and it becomes rebindable for free. `Scope` is how the graph takes `d`, `g` and `G`
while it is on screen — `Lookup` tries the view's scope before the global one — and it is a
property of the action rather than something the config can set, because where an action applies
is a fact about what it does. Phase 6 added two more scopes: `Diff` takes `s` for switching sides
and `Stash` takes `a`, `p` and `d`; `Branches` takes `x` for the prune, scoped because a network
call with a destructive edge should only be reachable from the one screen that shows what it would
remove, and `Ci` takes `b` for the ref picker, which means nothing anywhere else.
Two scoped bindings in different views never meet, so the
diff's `s` and the status view's `s` are not a conflict; a scoped key over a *global* one is
(the stash view's `p` over `pull`), and the checker only reports that when the config is what
arranged it.

**Reads are pure and return snapshots.** `ReadStatus`, `ReadHistory`, `FetchRepoInfo`,
`FetchPipelines`, `FetchJobs` and `FetchPulls` allocate their own results and touch no UI state,
which is what let the remote fetch move to a worker thread without changing anything else. One
thread must own a `Repository` at a time — libgit2 objects are not safe for concurrent use, which
is why `git/transfer.cpp` opens its own `git_repository` from a path rather than borrowing App's.

## Adding a view

A view is not one file, but Phases 5 and 6 removed four of the places it used to be.
`ui::AllViews()` in `panels.cpp` is the single ordered list, and the tab bar, the `tab`/`backtab`
cycle, the tab hit-boxes and now the digit keys all read it — adding an entry there gets all four.
The digit keys are `view_1` … `view_10`, positional actions that index `AllViews()`, so a new view
needs no key of its own and `[layout] views` can reorder the lot without the tab bar lying about
which number reaches what. `view_10` is bound to `0`, which is where the tenth key on a number row
actually is; `SlotKey` in `panels.cpp` is the one place that mapping lives, because the digit the
tab bar *prints* has to be the key that reaches it. An eleventh view would print no digit rather
than printing `11` and naming a key nobody has.

What is still manual: the `ui::View` enum, `TabLabel`, the `kViewNames` table (both directions at
once, so a view can never be parseable under a name it does not print), `ActiveSelection` and
`ActiveCount` for the selection state, `CurrentScope` if the view claims keys of its own, `SetView`
if it loads anything, `FilterTotal` and `RebuildFilter` if it has a list worth filtering, the render
tree, `Footer`'s per-view hints, and the help overlay. Miss one and the view exists but cannot be
reached, or scrolls the wrong list.

`TabBar` sheds width in tiers, and the tiers are three numbers — the spaces after the digit, after
the label, and between one tab and the next — fed to both the measurement and the drawing. That
sharing is the whole design. It used to be two independent formulas with a roomy tier and a
four-letter tier and nothing in between, so a bar one column too narrow for the first gave up forty
columns of padding *and* the second half of every word in the same step: "Remo" with an empty
right-hand third beside it. With the ten built-in tabs the ladder now lands on 129, 119, 109, 99,
89, 79 and 69 columns, and whole labels survive four steps of tightening before any of them is cut.

Two things to keep if you touch it. **Every term in `tier_width` has to be a term the loop below
actually emits** — dropping the label chip's leading space made it pick a tier ten columns too wide
and hand the difference to FTXUI, which clips mid-word and says nothing, which is the one outcome
the tiers exist to prevent. And **no tier may drop a tab**: a clipped bar hides that those views
exist at all, and the digit on a tab is the key that reaches it.

## The settings view

`ui/settings_panel.cpp` holds the page, and the one rule it exists to keep is that **the row list is
built in exactly one place**. `BuildSettings()` returns the groups; `SettingsRowCount` and
`SettingsRowAt` walk them, and App calls all three — to count rows for `ActiveCount`, to resolve
what `enter` does, and to draw. The list changes shape with the state it describes (a signed-in host
offers *sign out* where an anonymous one offers *connect*), so any second table of "row three is the
theme" would be wrong the first time somebody signed in. This is `AllViews()`'s lesson applied
again.

`SettingsView` carries what only App knows — the token source, the remotes, the config path, the
repository. It deliberately does **not** carry the theme, the glyph set, the border or the colour
depth: those are `ui/` state with their own accessors, and copying them through App would make the
struct a second place they could disagree with the thing actually on screen.

Three things about how it behaves:

- **Changes apply now and persist only when asked.** Every toggle takes effect immediately;
  `SaveSettings` is a row you press. That is not timidity — `Config::Save` regenerates the file from
  the keys gittop holds, so writing on every keystroke would eat a hand-written config's comments
  the first time somebody pressed `t` to look at a theme.
- **It writes `auto` back whenever the resolved value is what detection would have picked.**
  Pinning the resolved one would freeze *this* terminal's answer into a file that may be read on
  another: a dotfile carried to a 16-colour ssh session would arrive there demanding truecolor, and
  `theme.icons` would arrive demanding nerd glyphs a plain xterm has no font for.
- **A row that cannot be changed says why, in the column where its verb would have gone**, and
  keeps its note. The environment beating a sign-in is the case that matters: the note names the
  variable, the aside says the environment wins, and neither ever names the value.

`ColorDepthKey` exists because `ColorDepthName` is prose for a status line — "256 colors" does not
survive `ParseColorDepth`. Anything writing a depth into a config has to go through the key.

## Motion

Four settings, one flag, one cap.

`ui::ReducedMotion()` is asked by every moving thing — the eased bars, the spinners, the toast fade,
the skeleton shimmer, the CI pulse, the overlay reveal and the splash. Off makes each of them *snap
to its final state* rather than disappear: a reduced-motion setting that also removes information is
a worse setting than none, so a running CI row still gets its glyph and its tint, just a still one.

`App::ThrottleFrame()` is the frame cap, and it only ever runs on a frame the animation loop asked
for — `animation_pending_` is set beside the `RequestAnimationFrame` call and cleared on entry, so
an input-driven repaint is never delayed. A keystroke that waits 33ms for a progress bar is a
keystroke that feels broken. Measured on this machine: 33 frames drawn during the splash with the
cap, 65 without.

An idle dashboard still requests no frames at all — that is what `Animating()` is for and it is the
reason `remote::Ticker` exists. Do not add anything that repaints unconditionally.

The overlay arrival is the one transition a terminal can actually do. There is no compositor, so a
popup cannot slide or scale, but it can be drawn emerging from the background: `SetOverlayReveal`
ramps 0 to 1 over 120ms and `PaneFrame()` mixes the border and surface toward `bg` by it. One number
read in one place, rather than a `reveal` parameter threaded through seven panes.

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

**A deleted upstream is two states, and only one of them is visible offline.** A branch whose
remote counterpart was deleted keeps a remote-tracking ref until something prunes it, and while
that ref resolves nothing local can tell it apart from a healthy one — `git branch -vv` says
nothing either, so this is not gittop being uniquely blind. Once the ref *is* pruned, the branch
still has `branch.<name>.remote` and `.merge` in its config, and that gap is the signal:
`git_branch_upstream` resolves the ref and fails, `git_branch_upstream_name` reads only the config
and succeeds. `ConfiguredUpstream` in `repository.cpp` is that pair, and it is what turns "no
upstream" — which invites a `push -u` that recreates a branch somebody closed on purpose — into
`gone` with the dead upstream named. `git::Prune` is the other half: a fetch with
`GIT_FETCH_PRUNE`, on the Branches scope behind a confirm, which is the only thing that converts
the invisible state into the visible one. It reports by diffing the tracking refs across the
fetch rather than by subtracting counts, because a fetch adds refs as well as removing them.

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
it is the only one that changes something other people can see. Keep it that way. The rule the
confirms implement is that an operation asks first when it can destroy something a user cannot get
back — the working tree in a merge abort's hard reset, a branch in `Prune` — or when it is visible
to other people, which is push and only push. Anything new that can lose work joins that list.

## Diffs, stashes and interrupted operations

**The diff is one flat list of lines.** `model::DiffSnapshot::lines` holds file banners, hunk
headers and content rows in reading order, and `DiffFile::first_line` indexes back into it. That is
what makes scrolling a single integer; a cursor that is a (file, line) pair has to be kept agreeing
with itself through every fold and filter, and this one cannot disagree. The panel renders a window
around the cursor rather than every row, because twenty thousand dom nodes to show forty of them is
not free.

**Only a rebase has a continue.** A merge or a cherry-pick is finished by writing an ordinary
commit, so `ui::OperationPane` offers abort alone for those and says what to do instead — a `c` key
that answers "no rebase in progress" is worse than no `c` key. `RebaseAbort` is two operations under
one word: `git_rebase_abort` for a rebase, and a hard reset plus `git_repository_state_cleanup` for
everything else, which is what `git merge --abort` is. The second discards the working tree and its
confirm says so.

**`Repository::Commit` writes every parent, and this is not optional.** Committing during a merge
with one parent produces a commit claiming the other side never happened, leaves `MERGE_HEAD` on
disk, and looks like it worked — which is how it survived from Phase 1 to Phase 6 unnoticed. It
reads `MERGE_HEAD` through `git_repository_mergehead_foreach` and cleans the state up afterwards.
It also refuses during a rebase: those commits have to go through `git_rebase_commit`, which
advances the plan as well as the branch.

**Stash indices renumber.** `stash@{n}` is the only handle libgit2 offers and dropping one shifts
every entry below it, so every mutation sets `stashes_loaded_ = false` and re-reads rather than
adjusting the list it has.

## Filtering

`/` filters whichever list is on screen. App keeps one query and a filtered **mirror** of each
snapshot; `VisibleStatus()` and its four siblings return the untouched original when the query is
empty, so the common case copies nothing. Panels take the mirror and are otherwise unchanged, which
is the whole reason for this design over passing every list a vector of visible indices: the
selection is an index into what is on screen *by construction* rather than by five call sites
remembering to map through. `RebuildFilter()` is eager — called from `Refresh`, `EnsureStashes`,
every `Collect*` and every keystroke in the box — because a dirty flag is one more thing to forget
and the sources change on a keystroke, never on a frame.

A filtered commit list drops the lane gutter. Hiding the rows between two commits makes the gutter
draw connections to somewhere off screen, and a filtered log is a list, not a graph. The filter is
cleared on every view switch: it belongs to the list it was typed against, and the box explaining it
has already closed.

## The mouse and hit-testing

FTXUI cannot be asked where a dom node ended up: a node's box is only filled in during layout, and
the lists scroll inside a `yframe`, so a row's screen position has nothing to do with its index.
Every list therefore takes an optional `std::vector<ftxui::Box>*` and attaches `reflect()` to each
row; App matches a click against them on the next event, which works because FTXUI renders before
it reads input. A hit is also checked against the panel's own box, because a row scrolled out of
its frame still gets a box and it can land somewhere else on screen. Any new list wanting clicks
follows the same shape rather than computing rows from a y offset.

## Lazy reads and cache invalidation

Only the status snapshot is read at startup — plus `ReadOperation`, which is a handful of stat
calls and has to be in the same frame as the files it explains. The status read also carries
`ReadTracking` and `ReadRecent`, which feed the Status view's sidecar; both are *bounded*
(`git_graph_ahead_behind` stops at the merge base, `ReadRecent` abandons the walk after
`kRecentCommits`) and that bound is the only reason they are allowed there. Calling `ReadHistory`
to fill the same panel would put a full revwalk on every startup, which is the thing this section
exists to prevent. History is read on the first switch
to a view that needs it (`EnsureHistory`) and cached; the diff and the stash list likewise
(`EnsureDiff`, `EnsureStashes`); the remote, the CI runs and the pull requests are fetched on the
first switch to their tabs (`EnsureRemote`, `EnsurePipelines`, `EnsurePulls`), on worker threads.
Switching remotes with `R` drops the last three, because none of them describes the new one.
Opening a repository must never pay for a revwalk or an HTTP round-trip nobody asked to see.
Committing invalidates the history cache, `Refresh` invalidates the diff unless it is a commit's
(a commit cannot go stale), every stash mutation invalidates the stash list, and `r` forces a reload
of whichever view is active. Anything new that costs real time belongs on the same terms.

A request is also a cost the *user* did not ask for, which is why job drill-down is on `enter`
rather than on the cursor: following the selection would put a request on every keystroke. The
pull request detail pane is deliberately not the same — it costs nothing, since everything it shows
came back with the list, so moving the cursor keeps it open while moving off a CI run closes its
jobs. That asymmetry is the point, not an inconsistency. The CI
poll loop runs only while its view is on screen, only when authenticated, and stops below a fifth
of the remaining budget — and says which of those it is doing in the panel header, because a
dashboard that has silently stopped updating looks exactly like one where nothing is happening.

## The CI ref filter

**An empty run list is two different pieces of news and the header is what separates them.** The CI
view asks a provider for the runs on one ref, and that ref used to be the checked-out branch always,
derived from the status snapshot and unchangeable. gittop's own release workflow triggers on
tags, so every run it has is attributed to `v2026.08.1` and the like — which meant that standing on
`master`, the panel showed an empty state reading "no CI runs", indistinguishable from a repository
with no CI configured at all. `App::CiFilter` is the fix: `Branch` follows HEAD as it moves, `All`
sends no filter, `Ref` is pinned to something picked from `b`.

Three things hold it together.

- **`CiFilterRef()` is the only place the ref is decided**, and `HeadBranch()` the only place the
  detached-HEAD rule lives. A second copy of either is how the request and the header end up
  disagreeing about what was asked, which is the bug this section exists about.
- **`All` and a detached `Branch` send the identical empty filter and are not the same state.** One
  is a choice and one is a fallback, so `PipelineView::all_refs_pinned` carries the difference and
  the header draws them differently — accent for the choice, faint plus "no branch checked out" for
  the fallback. `ApplyRefPick` compares the *resolved* ref rather than the mode for exactly this
  reason: moving between those two on a detached HEAD changes a word on screen and nothing about
  the request, and refetching would spend rate budget to redraw it.
- **The ref list is `Repository::ReadRefs()`, read on the first `b` and re-read on `r`.** Branches
  and tags share one list because a provider's filter takes one string and does not care which kind
  it names — a tag-triggered GitHub run carries the tag in `head_branch`. Local and remote-tracking
  branches collapse to one row for the same reason: `origin/master` and `master` are one query.
  Tags sort newest-first by their peeled commit time, because on a tags-only workflow the newest tag
  is the whole reason the list was opened; branches sort alphabetically, because that is how a
  branch gets looked up.

`RunsEndpoint` needed no change at all — it has always omitted the filter for an empty string, since
that is the path a detached HEAD already took.

## Config

Keys are flat dotted paths with quotes stripped, so `hosts."gitlab.internal".token` is looked up as
`hosts.gitlab.internal.token` and reached through `HostValue(host, field)`. Per-host settings are
table keys rather than code, which is what lets a self-hosted instance be configured without a
patch. The reader accepts comments, table headers, and `key = value` for strings, ints, and bools —
that is the entire language. It is a strict subset rather than a lookalike, so a real TOML parser
can be dropped in later without migrating anyone's file.

`[layout]`, `[theme]`, `[theme.colors]` and `[keys]` are read in `App::ApplyConfig`, which runs in
the constructor so nothing is ever drawn in a palette the user replaced — and `[layout] views` has
to be applied there too, before anything reads `AllViews()`, since the digit keys are positional. It scans by key prefix rather
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

## Colour and accessibility

The seven curated palettes were measured against a Viénot/Brettel dichromat simulation in Phase 7
and most of them fail on the status colours: staged against conflicted is **CIELAB ΔE 0.8** under
deuteranopia in the default theme, and daylight's unstaged against its conflict is 0.7. Those are
the same colour. This is not a bug in the palettes — they are Catppuccin's and Gruvbox's real
published colours and retuning them would make them not those themes — and gittop stays readable
because a status is drawn as a **glyph and a letter** as well as a colour. That rule is load-bearing,
not decorative; it is why `Decorate()` returns three things.

`theme.name = "accessible"` is the palette for when the colour should work rather than merely be
redundant: the four states move off the red/green axis onto blue/amber, separated by lightness as
well as hue, worst case ΔE 36.6 across normal vision, deuteranopia, protanopia and tritanopia. Its
six graph lanes are *not* all mutually distinct and cannot be — a dichromat sees a roughly
two-dimensional colour space and six separated hues do not fit in it. That is acceptable where it is
not for the statuses, because a lane's colour is redundant with its column.

If you add a palette, run the four status roles through a simulation before shipping it.

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

`App::session_token_` is the third, added with sign-in, and it is the only secret gittop
deliberately *writes*. It is scoped to the host that issued it — a `SessionToken` carries its host
and `MatchesHost` is checked before it is offered, because a token handed to the wrong instance is
a leaked token rather than a failed request. `ui::SignInPane` takes an already-rendered password
`Input` for the same reason `PassphrasePane` does. The `device_code` is treated as a secret too:
it is the bearer of the pending token for the length of the flow, so it lives on App and never
crosses into `ui/`.

## Signing in

`L` opens one overlay with two routes into it, and which one is not a preference: the OAuth device
flow when the host has a `client_id`, the guided personal access token when it does not.

**The device grant is the only OAuth flow that fits.** An authorization-code flow needs a loopback
server listening for a redirect — a port, a firewall prompt, and a race with whatever else is on
that port — to do what two POSTs already do. It is also the only one that works over ssh, which is
a normal way to reach a machine you want a dashboard on.

**`remote/oauth.cpp` ships no client_id, and that is deliberate.** A device-flow id is public by
design — there is no client secret, which is what makes it safe in a distributed binary — but it
binds the build to one registered application, so filling `kGitHubClientId` in is a packaging
decision. Until somebody does, every host takes the token route, which is why that route is a
first-class path and not an error state. `hosts."<host>".client_id` is the only way a self-hosted
instance can ever have one.

**A pending authorization is HTTP 200 on GitHub and HTTP 400 on GitLab.** Both send
`{"error": "authorization_pending"}`; only GitLab is following RFC 8628. `PollDeviceFlow` therefore
decides on the *body* and never on the status code — deciding on the code instead makes every
GitLab sign-in fail on its first poll, and it will look like a rejected sign-in.

**The OAuth origin is not the API base and not `https://<host>`.** github.com issues device codes
while api.github.com answers everything else here, so it cannot come from `ref.api_base`; and
`RemoteRef::host` has its port stripped (no API base wants one), so a self-hosted instance on a
non-standard port would have its device request sent to port 443 of the same name.
`hosts."<host>".oauth` overrides it, and is what makes the flow testable against a local stub.

**Polling is a chain, not a timer.** `CollectPoll` starts the next poll, and the interval is waited
out *inside* the task in 50ms slices, where the cancel flag already reaches. There is no second
thing to remember to stop, and esc breaks the chain by cancelling the fetcher — a re-arming poll
that outlives its overlay would poll until the process died.

Two things it must keep doing: `ScopesFor` includes write to the repository (`repo`,
`write_repository`) because this token is also what authenticates an https push, and a read-only
one would leave `P` failing with a 403 that looks like a gittop bug. And `OpenInBrowser` forks and
execs with an argv — the URL came off the network, and a server response reaching `system(3)` is a
command injection with extra steps — with the child's stdio on `/dev/null`, since the opener's
chatter would otherwise land on the alternate screen.

One accepted cost: `Config::Save` regenerates the file from the keys gittop is holding, so a
sign-in **drops the comments** out of a hand-written config. The template says so.

## FTXUI gotchas already paid for

- `canvas(fn)` looks like it auto-fits its box; it hardcodes 12×12. Size canvases from
  `screen.dimx()`.
- The canvas callback runs during Render, *after* the building function has returned. Capture
  everything by value.
- The dom cannot query its own size. `screen.dimx()` / `dimy()` are read inside the Renderer
  lambda and passed down, which is how every responsive breakpoint works.
- `flex` belongs on the panel itself, not on a `vbox` wrapped around it. Wrapping stretches the
  container and leaves the box at its content size.
- **FTXUI quantizes colour a second time, and it does not ask `ToColor`.** It computes its own
  support from `TERM`/`COLORTERM` and downgrades whatever it is handed. While the two disagreed,
  `theme.depth = "truecolor"` was a setting that visibly did nothing — gittop stopped quantizing
  and FTXUI carried on. `ui::SetColorDepth` now pushes the resolved depth into
  `ftxui::Terminal::SetColorSupport` so one decision reaches both. Anything else that reasons about
  colour depth has to go through that function, not around it.

Two palettes that differ in 24-bit can land on the *same* 256 index — `default` and `catppuccin`
used to render byte-identical frames — so a theme switch on an eight-bit terminal looks like a
broken key rather than a subtle change. That is why `DetectColorDepth` now recognises `WT_SESSION`
(Windows Terminal, and therefore most WSL shells, which set no `COLORTERM`) and treats a `-direct`
terminfo entry as 24-bit rather than as 256, and why the `t` toast names the depth whenever it is
below truecolor. Guessing low is not free.

## Releases

Versions are CalVer, `YYYY.MM.PATCH`, and live in exactly one place: `project(gittop VERSION ...)`
in `CMakeLists.txt`. Everything else reads it from there — `--version`, CPack's package name, and
the tag check.

Cutting one is four steps, and the workflow only starts at the last:

```bash
# 1. bump project(gittop VERSION ...) in CMakeLists.txt
# 2. add a "## <version> — <date>" section to CHANGELOG.md
scripts/changelog-section.sh 2026.08.5      # 3. prove the section extracts non-empty
git tag v2026.08.5 && git push origin v2026.08.5
```

Step 3 is the one worth not skipping. `.github/workflows/release.yml` uses that script's output as
the release description, and an extractor that returns nothing publishes an empty release — the
script exits non-zero on an empty section precisely so the failure happens before the tag exists.
The workflow separately refuses a tag whose version does not match `CMakeLists.txt`, so a forgotten
bump is caught rather than shipped.

Two things CI enforces that a local Debug build will not tell you about:

- **A glibc floor of 2.35** (Debian 12 / Ubuntu 22.04). The job reads the highest `GLIBC_*` symbol
  out of `objdump -T` and fails above it, so a dependency or a newer libc function can break the
  release from a change that built and ran perfectly here.
- **The package has to install and run.** It is installed with `apt` rather than `dpkg -i`, which
  proves the `Depends` field is satisfiable; `dpkg` would unpack it regardless and leave an install
  that still runs on the build machine and nowhere else.

The release build is `-DCMAKE_BUILD_TYPE=Release` and packages with `cpack -G DEB` and `-G TGZ`
from the build directory. `packaging/gittop.desktop` goes through `desktop-file-validate`.

The README's GIF is reproducible and both halves are checked in: `scripts/demo-repo.sh` builds the
throwaway repository (gittop's own history makes a bad demo — the heatmap is one cluster at the
right edge), and `demo.tape` is recorded against it. Neither touches the network, and the demo
repository is gitignored.

## Conventions

- **License: GPL-3.0**, but sources carry no per-file header; do not start adding them.
- Two-space indent, `-Wall -Wextra -Wpedantic` clean.
- Comments explain *why*, especially where a simpler-looking approach was tried and failed. Match
  that density rather than annotating what the code already says.
- Remote: `git@github.com:sinhaparth5/gittop.git`, default branch `master`.
