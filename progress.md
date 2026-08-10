# gittop — Progress

A btop-inspired terminal dashboard for Git: local repo state that always works offline, plus
remote-aware CI/pipeline and PR/MR panels for GitHub and GitLab.

**Status:** Phases 0–5 complete and running. Next: Phase 6 (diff viewer, stash, rebase helpers).
**Started:** 2026-08-09
**Last updated:** 2026-08-10

---

## Locked decisions

| Area | Choice | Notes |
|---|---|---|
| Name | **gittop** | Matches repo + remote `git@github.com:sinhaparth5/gittop.git`. Not re-litigating. |
| Language | C++20 | |
| TUI | FTXUI | Declarative, good fit for boxed layout + custom canvas graphs |
| Local Git | libgit2 | No shell dependency, no output-format churn across git versions |
| HTTP | libcurl | |
| JSON | nlohmann/json | |
| Auth | Personal Access Tokens | Env var first, config file second |
| Config | `$XDG_CONFIG_HOME/gittop/config.toml` | `$GITTOP_CONFIG` overrides; `--config` overrides that. Created 0600 |
| Config format | A strict TOML subset | Comments, tables, string/int/bool. Files it reads are valid TOML, so a real parser drops in later without migrating anyone |
| Build | CMake + FetchContent | libcurl comes from the system; see the note under Phase 3 |
| License | GPL-3.0 | Already in repo; new sources get GPL-3.0 headers |

Anything not in this table is still open — see [Open questions](#open-questions).

---

## Target structure

Not created yet. This is the shape to grow into, not a directory to scaffold empty:

```
gittop/
├── CMakeLists.txt
├── src/
│   ├── main.cpp
│   ├── git/          # libgit2 wrapper: status, log, branches, staging, commit
│   ├── remote/       # provider-agnostic interface + github/, gitlab/ clients
│   ├── model/        # Status, Commit, Pipeline, PullRequest — provider-neutral types
│   ├── ui/           # FTXUI components, one file per panel
│   ├── viz/          # braille/block graph primitives, heatmap, progress bars
│   └── config/       # config file, tokens, theme, keybindings
├── tests/
└── progress.md
```

**Key boundary:** `model/` types are provider-neutral. GitHub Actions and GitLab CI have
genuinely different shapes (workflow runs → jobs vs. pipelines → stages → jobs); the
normalization happens in `remote/github/` and `remote/gitlab/`, never in `ui/`. If a UI file
ever needs to know which provider it's talking to, the abstraction leaked.

---

## Roadmap

> **Note on Phase 7.** The big visual pass is deliberately last, but it only stays a *restyling*
> job if panels never hardcode a color, glyph, or border style. Phase 1 ships a minimal theme
> token layer (`ui/theme.hpp`) and every panel pulls from it. Get that wrong and Phase 7 becomes
> a rewrite of every file in `ui/` — the same trap as the async seam.

### Phase 0 — Project setup ✅
- [x] `CMakeLists.txt` with C++20, FetchContent for FTXUI v7.0.3 and libgit2 v1.9.6
- [x] FTXUI screen that renders and exits cleanly on `q`
- [x] libgit2 wired in and linking (in-tree target is `libgit2package`)
- [x] Detect whether cwd is inside a Git repo; friendly message if not
- [x] `build/` ignored

**Decided here:** libgit2 is vendored rather than taken from the system, because
`libgit2-dev` was not installed and installing it needs root. It builds with `USE_HTTPS=OFF`
and `USE_SSH=OFF` since Phase 1 never touches the network; both flip on in Phase 5, and the
SSH side is the one to budget time for. `GITTOP_SYSTEM_LIBGIT2` is not wired up yet.

*Settled in Phase 5, and the SSH side cost nothing in the end.* `USE_HTTPS=OpenSSL-Dynamic` and
`USE_SSH=exec` both turned on without adding a single build dependency — see the Phase 5 notes.
The budgeted afternoon of libssh2 wrangling never happened because gittop does not link libssh2
at all; it runs the `ssh` the user already has.

### Phase 1 — Local status dashboard ✅
- [x] Read status: staged / unstaged / untracked / conflicted
- [x] Status cards with counts and gradient fill bars
- [x] Scrollable file list with selection, grouped by state
- [x] Stage / unstage selected file (`space`, `s`, `u`), stage everything (`a`)
- [x] Discard changes behind a confirm dialog; restores from the index, matching `git restore`
- [x] Commit: message input overlay → `git_commit_create`
- [x] Keybinding help overlay (`?`)
- [x] Theme token layer — semantic names only, one palette behind them

**Verified end to end:** stage-all then commit produces a real commit and leaves a clean tree;
answering `n` to a discard keeps the file; a non-repository path exits 1 with a readable message.

### Phase 2 — History & visualization ✅
- [x] Commit log panel, capped at 400 rows from a walk bounded at 6000 commits
- [x] Commit graph in box-drawing lanes, six cycling lane colors
- [x] Activity heatmap over a 91-day window, laid out as 13 weeks by 7 weekdays
- [x] Branch list with ahead/behind against upstream, HEAD pinned first
- [x] Responsive layout: stat cards stack 2×2 under 84 columns, heatmap steps
      aside under 30 rows
- [x] Four views with a tab bar (`1`…`4`, or `tab` to cycle)
- [x] Graph view: braille area chart of commits over time, pannable with `h`/`l` and
      switchable between day, week and month buckets with `d`/`w`/`m`, plus panels for top
      authors, weekday distribution, and a commits-by-hour sparkline

**Verified against git itself:** the lane structure matches `git log --graph --all` on a repo
with a feature branch, a merge, and a dangling branch; the branch panel matches
`git for-each-ref` including `↑2` on an ahead branch.

**Known simplification:** a lane that closes or opens more than one column away from its commit
is drawn as a single `╯` or `╮` rather than a run of horizontals. Git draws `|_|_/` in that
case. It reads correctly for the common shapes and gets terse for octopus merges.

**Deliberate:** history is read lazily on first switch to a history view and cached, so opening
a repository never pays for a revwalk nobody looked at. Committing invalidates the cache, and
`r` forces a reload.

### Phase 3 — Remote foundation ✅
- [x] Config file at `$XDG_CONFIG_HOME/gittop/config.toml`, load and save, 0600
- [x] `--init-config` writes a commented starter; `--config-path` prints where it looks
- [x] Token resolution: env var → config file → unauthenticated
- [x] Parse remote URL → GitHub / GitLab / self-hosted / unknown, with a config override
      for a host whose name gives nothing away
- [x] libcurl client with timeouts, retry with backoff, and cancellation
- [x] Async fetch layer — the UI thread never blocks on the network
- [x] Remote view (tab `5`): identity, star/fork/issue counts, API budget, details

**libcurl is taken from the system, not vendored.** It is the one dependency where the
distribution's build is the one to want: it ships configured for the platform's CA bundle and TLS
backend, and a self-built copy would have to be told where the system's certificates live before
it could verify a single request. `find_package(CURL 7.68 REQUIRED)`. nlohmann/json comes from the
release tarball rather than a clone — a few hundred kilobytes against three orders of magnitude
more, nearly all of it test suite.

**The async seam held.** The thing the Phase 0 note warned about did not happen: `ReadStatus` and
`ReadHistory` stayed synchronous and nothing about them had to change, because the fetch that
actually needed a thread was new code written against the seam rather than retrofitted through it.
The worker posts `Event::Special("gittop:remote-ready")` and the result is picked up in the root
`CatchEvent`, on the UI thread, where every other piece of state is already touched.

**Verified against a stub API as well as the real one:** the GitHub path against
`api.github.com` (anonymous, 60/hour budget shown correctly), and the authenticated GitLab path
against a local stub, which confirmed the `PRIVATE-TOKEN` header rather than `Authorization`, the
project path encoded as one segment (`/projects/eng%2Fpayments-service`), GitLab's field names
normalized into the same `RepoInfo` GitHub fills, and `RateLimit-*` headers read. A 503 stub
confirmed three attempts at 300ms and 600ms; a 401 stub confirmed one attempt and no retry.
Quitting mid-fetch exits in about half a second rather than waiting out the ten-second timeout.

**Token discipline, enforced not just intended:** the token lives in `remote::Token`, which
nothing under `ui/` takes. The panel names the variable or the file it came from and there is no
code path that prints the value, masked or otherwise — checked by grepping a full session capture
for the test token. A remote configured as `https://user:token@host/...` has its userinfo replaced
before the URL reaches the screen.

### Phase 4 — Pipelines & CI ✅
- [x] GitHub: list workflow runs for current branch
- [x] GitLab: list pipelines for current branch
- [x] Normalize both into `model::Pipeline`
- [x] Status indicators: success / failed / running / pending / cancelled
- [x] Job-level drill-down
- [x] Live refresh on interval, respecting rate limits
- [x] Rate-limit display (GitHub exposes remaining/reset headers)

**The `model::Pipeline` warning from the risks list was the real work.** GitHub splits a run's
state across two fields — `status` while it is alive, `conclusion` only once it is not — so nine
conclusions and six statuses fold into one enum; GitLab answers with one field and eleven values.
Reading `conclusion` alone reports every in-flight run as unknown, which is the bug the two-field
shape is there to cause. `RunStatus` has eight values because that is what a person can actually
tell apart on a dashboard: GitHub's `neutral` is folded into skipped, since GitHub itself greys
them identically and pretending to distinguish them would be a lie the colour cannot tell.

**What each provider does not send is as much of the design as what it does.** GitLab's pipeline
list carries no commit title and no author — both need a request per pipeline — so those fields
stay empty and the panel omits them, exactly as `RepoInfo::watchers` does. GitHub has no stage
concept, so the jobs pane grows a stage column only when something fills it. GitLab reports job
`duration` itself and it is the better number: it excludes time spent queued, which a derived
finished-minus-started does not.

**Polling had to be designed against the rate limit, not bolted to a timer.** Auto-refresh runs
only while the CI view is on screen, only when there is a token, and stops below a fifth of the
remaining budget — anonymous GitHub gets sixty requests an hour and a twenty-second poll would
spend it in twenty minutes. Every one of those states says so in the panel header rather than
looking stuck. Jobs are fetched on `enter` rather than following the cursor, because following it
would put one request on every keystroke; a refresh under an open drill-down re-reads jobs only
while the run is unfinished.

**A ticker thread, not animation frames.** An idle dashboard requests no frames, so nothing would
notice an interval elapsing. Driving it with `RequestAnimationFrame` would mean repainting at
sixty hertz for twenty seconds to watch a clock. `remote::Ticker` posts one event a second while
the CI view is open, which also lets the countdown be honest.

**Verified against a stub for both providers and against the live API.** Both status foldings
including GitHub's `neutral` and a null `conclusion`; the project path as one encoded segment
(`/projects/eng%2Fpayments-service/pipelines?ref=master`) against GitHub's
`/actions/runs?branch=master`; `PRIVATE-TOKEN` versus `Authorization: Bearer`, logged by header
name only; GitLab's newest-first jobs re-sorted into execution order and its own `duration` used;
a detached HEAD dropping the branch filter and saying "all branches"; anonymous and
budget-below-20% both pausing the poll with a reason on screen; three run-list requests in a
26-second visit at a ten-second interval; an unreachable API rendering as an error panel with a
hint; and quitting mid-fetch returning in about a second rather than waiting out the ten-second
timeout. Live, anonymous, against `api.github.com`: `sinhaparth5/gittop` has no workflows, which
draws the designed empty state.

**Known simplification:** one page of runs, thirty of them, and no pagination. GitLab's list gives
no way to tell a pipeline's own duration from its created/updated pair when it is still running,
so a running GitLab pipeline's elapsed time is measured from creation and includes any time it sat
queued.

### Phase 5 — Full remote features ✅
- [x] Pull Requests / Merge Requests panel (tab `7`)
- [x] Push / pull with progress and auth handling
- [x] Multiple remotes, cycled with `R`
- [x] Themes: seven palettes, a light one, user palettes from config, 256/16/mono fallback
- [x] Mouse support: wheel, click to select, click a tab, click again to open
- [x] Configurable keybindings under `[keys]`

**The transports came back on with no new build dependency, which was luck worth
taking.** libgit2 has been built with `USE_HTTPS=OFF USE_SSH=OFF` since Phase 0, and the note
there said turning them on was the thing to budget time for. The two backends chosen avoid the
usual cost entirely. `OpenSSL-Dynamic` dlopen()s libssl at run time, so the build needs no OpenSSL
headers and the binary is not welded to the 1.1 or 3.x it compiled against. `USE_SSH=exec` runs the
system `ssh` instead of reimplementing it through libssh2 — cheaper to build, and better behaved:
gittop inherits `~/.ssh/config`, the agent, the user's keys and their known_hosts, so a remote that
works from the shell works here with no second setup. `git_libgit2_features()` reports both at run
time and the remote panel prints them, because whether push can work at all is a property of the
binary rather than of the repository.

**A network gave libgit2 a way to hang the program, and the cancel flag could not reach it.**
A transfer checks for cancellation only from libgit2's progress callbacks, and a server that
accepts a connection and then says nothing never fires one — so neither `esc` nor `q` could get
out, because quitting joins the worker. Verified against a socket that accepts and stays silent:
gittop had to be killed. `GIT_OPT_SET_SERVER_CONNECT_TIMEOUT` and `GIT_OPT_SET_SERVER_TIMEOUT`, set
in `git::Library` to the same 5s/10s budget libcurl already runs with for the REST calls, are the
only thing that bounds that wait. Afterwards: 64ms to quit normally, 9.6s worst case out of a
stalled transfer. `q` inside the progress pane means "cancel this and let me out" rather than
nothing at all, and the exit happens when the worker reports back rather than in the keypress.

**`git_remote_push` returns zero for a push the server refused.** The refusal arrives through
the `push_update_reference` callback and nowhere else, so a push that does not check it reports
success for a rejected non-fast-forward. Confirmed both ways against a `git daemon` on loopback
with a `pre-receive` hook that declines: the toast reads "origin refused the push — refs/heads/
master: pre-receive hook declined" and no ref was created. This is not reachable with a local-path
remote at all — libgit2's local transport copies objects directly and never runs receive-pack —
which is why the daemon was worth standing up.

**Pull fast-forwards or refuses, and never does anything else.** A diverged branch needs a merge
commit or a rebase, both of which are Phase 6; doing either implicitly behind a key called "pull"
is how a tool loses work that exists nowhere else. The checkout is `GIT_CHECKOUT_SAFE`, so a
fast-forward that would write over an uncommitted change stops and says which file is in the way.
Push is never forced — no `+` on the refspec — and it is the only operation that asks first,
because it is the only one that changes something other people can see.

**Two kinds of shadowing had to be told apart in the keymap.** Routing now goes through one
action table so a config can move a key, and the graph deliberately takes `d`, `g` and `G` away
from the shared bindings while it is on screen. A first pass reported those as conflicts on every
single startup. The rule that works: same-scope collisions are always conflicts, a scoped key over
a global one is a conflict only when the config is what put it there, and the message names which
action goes missing rather than just listing both.

**Colour depth is one function, not a branch in every panel.** Every colour in the program
already funnelled through `ToColor`, so the 256-colour cube, the sixteen-colour fallback and
`NO_COLOR` are a quantizer in that one place and no panel knows about any of it. Palettes are
authored in sixteen roles and composed into the thirty semantic tokens, which is what makes a user
palette in the config the same object as a built-in one and a new theme sixteen lines. Verified at
all four depths; under `NO_COLOR` the glyph-and-letter rule from Phase 1 is what keeps the status
list readable with no colour at all.

**FTXUI cannot be asked where a node ended up, so the mouse needed `reflect()`.** A node's box
is only filled during layout, and the lists scroll inside a frame, so a row's screen position has
nothing to do with its index. Every list now reflects its rows' boxes and App matches a click
against them on the next event — which works because FTXUI renders before it reads input. A click
is also checked against the panel's own box, since a row scrolled out of its frame still gets a box
and it can land somewhere else on screen.

**Verified end to end.** Both providers' pull request shapes against a stub, including the four
things only one of them sends (GitHub has no mergeability or comment count on a list; GitLab has
no `merged_at` distinction and folds `locked` into open), GitLab's `detailed_merge_status` and the
older `merge_status` both read, and the branch you are standing on sorted to the top and marked.
Fetch, pull and push against local-path remotes and against a `git daemon` on loopback: 60 commits
fetched with a live progress bar, a fast-forward that moved the working tree, a no-op pull, a
diverged pull refused with HEAD unmoved, a fast-forward refused because of an uncommitted edit with
the edit intact afterwards, a non-fast-forward push refused client-side, a push refused
server-side, and `git push -u` setting the upstream. Themes at truecolor, 256, 16 and none; five
different malformed config lines each reported by name; keys rebound and the footer and help
overlay following them. Mouse driven with SGR sequences: a tab click, a row click, a second click
opening the detail pane, and wheel notches moving three rows each way.

**Known simplifications.** Open pull requests only, one page of thirty, no pagination — the same
terms the CI list is on. Mergeability and comment counts stay empty on GitHub because its list
endpoint does not report them and asking per row would be one request each. Pull is fast-forward
only. There is no force push and no way to ask for one. The push confirmation names the remote and
the branch but does not show what is about to be sent, so it cannot tell you that you are about to
push forty commits rather than one.

### Phase 6 — Power features
- [ ] Diff viewer
- [ ] Stash management
- [ ] Interactive rebase helpers
- [ ] Search / filter across panels
- [ ] Custom layouts

### Phase 7 — Visual design pass
The phase where gittop stops looking like a functional TUI and starts looking like something
people screenshot. Everything below is restyling, not rebuilding — which only holds if the
token layer from Phase 1 was done properly (see note under the roadmap).

**Landed early, during the Phase 1 polish pass:** semantic tokens with a three-step surface
scale, gradient-ramped bars drawn with eighth-blocks in a custom FTXUI node, glyph-plus-letter
status so rows read without color, grouped file list with per-group headers, dimmed scrim
behind overlays, key chips, a designed empty state, dimmed directory / bright filename paths,
eased bar animation on an 80ms time constant driven by `RequestAnimationFrame`, and a toast
that fades on a reserved line so nothing reflows under the cursor. The rest below stands.

**Design language**
- [ ] Semantic color tokens finalized (`accent`, `success`, `danger`, `muted`, `surface`, `border`, …) — no raw colors at call sites
- [ ] Spacing scale (1/2/4 cells) applied consistently; audit every panel for off-by-one padding
- [ ] Type hierarchy: bold/dim/italic used systematically, not ad hoc
- [ ] Tabular alignment for all numeric columns so digits don't jitter on refresh

**Color & themes** — landed in Phase 5, since themes were on that list anyway and doing them
twice made no sense. What is left here is the audit, not the mechanism.
- [x] Truecolor palette with graceful 256-color and 16-color degradation
- [x] Terminal capability detection (`COLORTERM`, `TERM`) driving the fallback
- [x] Built-in themes: default (btop-flavored), Catppuccin, Gruvbox, Nord, Tokyo Night, Dracula
- [x] At least one light theme that genuinely works, not an inverted dark theme
- [x] User themes loadable from config
- [x] `NO_COLOR` env var respected

**Glyphs & borders**
- [ ] Nerd Font icon set for file states, branches, CI status — with an ASCII fallback mode
- [ ] Rounded / double / heavy border styles, selectable
- [ ] Panel titles with accent color and focus-aware styling
- [ ] Focused panel visually unmistakable (border accent + title emphasis)
- [ ] Correct display-width handling for CJK and emoji so borders never tear

**Graphs & indicators**
- [ ] Braille canvas renderer for the commit-activity graph
- [ ] Gradient-filled progress bars (btop-style color ramp across the fill)
- [ ] Sparklines for per-branch commit velocity
- [ ] Heatmap with a proper sequential ramp, not 5 hardcoded greens
- [ ] Animated spinner for running CI jobs; pulse effect on in-progress pipelines
- [ ] Status conveyed by glyph *and* color — never color alone

**Motion**
- [ ] Frame-rate cap with dirty-region redraw (target: no full repaint on idle)
- [ ] Eased transitions on bar/graph value changes instead of hard jumps
- [ ] Panel enter/exit and popup transitions
- [ ] Reduced-motion config flag that disables all of it

**The details that actually sell it**
- [ ] Startup splash / ASCII logo (skippable, and skipped when not a TTY)
- [ ] Designed empty states — "no pipelines yet" should look intentional
- [ ] Loading skeletons instead of blank panels during async fetch
- [ ] Graceful text truncation with ellipsis; never mid-glyph
- [ ] Consistent, non-jarring error/toast presentation
- [ ] Demo GIF via VHS or asciinema for the README

**Verification**
- [ ] Render check across kitty, alacritty, wezterm, GNOME Terminal, tmux, and plain `TERM=xterm-256color`
- [ ] Colorblind-safe check on the status palette (deuteranopia/protanopia)
- [ ] Frame time measured under a large repo, not a toy one

---

## Risks & things to decide before they bite

**libgit2 via FetchContent is heavier than it looks.** It pulls zlib, and needs a TLS backend
(OpenSSL or mbedTLS) plus libssh2 for SSH remotes. The repo's own remote is SSH, so push/pull in
Phase 5 needs SSH support compiled in — that's a build-config decision to make in Phase 0, not
Phase 5. Alternative: find_package a system libgit2 and only FetchContent the header-only-ish
deps. Decide early; retrofitting is painful. *Resolved in Phase 5, and the premise turned out to
be wrong: neither backend needs a dev package. The retrofit was one CMake edit, because the
decision this warned about — whether to vendor libgit2 at all — had already been made correctly.*

**A network transport can hang a worker where a cancel flag cannot reach it.** Found in Phase 5:
libgit2 only checks for cancellation from its progress callbacks, so a socket that accepts and then
goes quiet is unreachable and quitting joins the worker. Bounded now by libgit2's own connect and
idle timeouts, set alongside the library init. Any future long-running libgit2 call needs the same
question asked of it: what fires the callback that notices the cancel?

**Async is a Phase 3 problem but a Phase 0 design constraint.** *Settled in Phase 3, and the
warning turned out to be worth heeding.* The one thing to remember for Phase 4: the notifier
captures the `ScreenInteractive` by reference, so `Fetcher::Shutdown()` has to run before that
screen is destroyed. `App::Run` calls it straight after `Loop()` returns. Any future worker that
posts events needs the same treatment.

**Destructive operations need guardrails.** Discard, force-push, and rebase helpers can lose
work. Every one of them gets an explicit confirm step, and nothing gets a single-keystroke path.

**Token handling.** Never log tokens, never render them in the UI (not even masked-with-a-reveal),
and config files get `0600`. Env var should win over the config file so CI/ephemeral use doesn't
require writing secrets to disk.

**GitHub vs GitLab API divergence.** GitHub Actions: workflow runs → jobs → steps. GitLab CI:
pipelines → stages → jobs. Different pagination, different auth headers, different rate-limit
semantics. The `model::Pipeline` type needs to be designed against *both* API shapes up front,
not designed for GitHub and then patched for GitLab.

---

## Open questions

- Minimum terminal size to support, and behavior below it
- Test strategy: unit tests against fixture repos created at test time? Which framework (Catch2 / doctest / GoogleTest)?
- Target platforms — Linux only initially, or macOS too?

---

## Work log

Newest first. One line per session: what changed, what's next.

- **2026-08-10** — Phase 5 done. Pull requests and merge requests on tab 7, push/pull/fetch on a
  worker with a progress overlay, remotes cycled with `R`, seven themes with 256/16/mono fallback,
  configurable keybindings, and the mouse. Four things came out of it. The transports the Phase 0
  note said to budget time for cost nothing: `OpenSSL-Dynamic` needs no OpenSSL headers and
  `USE_SSH=exec` needs no libssh2, and the second is better behaviour rather than a compromise
  since it inherits the user's own ssh setup. Giving libgit2 a network reintroduced a hang the
  async layer thought it had solved — the cancel flag is only read from progress callbacks, so a
  silent server is unreachable — and only libgit2's own timeouts bound it. `git_remote_push`
  returns zero for a push the server refused, which took a `git daemon` with a declining hook to
  prove, because a local-path remote never runs receive-pack at all. And routing every key through
  an action table meant teaching the conflict checker the difference between the graph
  deliberately shadowing `g` and a config accidentally doing the same thing. Next: Phase 6.
- **2026-08-10** — Phase 4 done. CI view on tab 6: workflow runs and pipelines for the current
  branch, both folded into `model::Pipeline`, a job drill-down on `enter`, and a refresh interval
  that stops itself when anonymous or when the budget runs low. Three things came out of it. The
  provider-shared parts of client.cpp — percent-encoding, ISO 8601, rate-limit headers, auth
  headers, status-to-sentence — moved to `remote/api.cpp` the moment a second endpoint family
  needed them; duplicating `DescribeStatus` would have been the start of two of them drifting.
  `remote::Fetcher` became a template over its result type rather than gaining two near-copies,
  and App now holds one per concern so a timer refresh can never sit in front of a fetch the user
  asked for. And the refresh interval needed a ticker thread, because the whole point of the
  animation-frame arrangement is that an idle screen asks for no frames and therefore cannot
  notice a clock. Next: Phase 5.
- **2026-08-09** — Phase 3 done. Config, tokens, provider detection, a libcurl client with
  retry and cancellation, the async fetch layer, and the Remote view on tab 5. Two things
  learned: FTXUI's `flex` has to go on the panel itself rather than a `vbox` wrapped around
  it, or the container stretches and the box keeps its content size; and the token's origin
  is a whole config path long, which will push a hostname clean off its own header if it is
  put there — it belongs in the details list. Next: Phase 4.
- **2026-08-09** — Added the Graph view (tab 4): braille area chart over the full commit
  timeline, pannable and bucketable. The daily series is now built across all walked history
  rather than only the heatmap window. Two things worth remembering: FTXUI's `canvas(fn)`
  overload looks like it auto-fits its box but hardcodes 12×12, so the canvas is sized from
  `screen.dimx()`; and the canvas callback runs during Render, after the building function has
  returned, so anything it touches must be captured by value.
- **2026-08-09** — Phase 2 done. Three views behind a tab bar, commit graph with lane
  assignment split into `src/git/graph.cpp` as a pure function over the commit list, activity
  heatmap, branch panel with ahead/behind. Responsive layout reads `screen.dimx()`/`dimy()` in
  the render lambda, since the dom itself has no way to ask. Next: Phase 3.
- **2026-08-09** — Visual pass over the Phase 1 screen: gradient bars, grouped list, eased
  animation, toast fade, overlay scrim. Fixed two event-routing bugs along the way, both the
  same root cause — `Container::Stacked` and `Container::Tab` only deliver events to a focused
  child, and the only focusable component in the tree is the commit `Input`. All key routing
  now lives in one handler on the root component. Next: Phase 2.
- **2026-08-09** — Phases 0 and 1 done. C++20 / FTXUI v7.0.3 / libgit2 v1.9.6, building clean
  under `-Wall -Wextra -Wpedantic`. Local staging, discard, and commit all work against a real
  repository. Next: the visual pass.
- **2026-08-09** — Added Phase 7 (visual design pass); Phase 6 renamed to "Power features" to
  keep functional work distinct from visual work. Theme token layer pulled forward into Phase 1
  so Phase 7 stays a restyle.
- **2026-08-09** — Repo initialized (README, GPL-3.0 LICENSE, CMake `.gitignore`). Plan locked,
  `CLAUDE.md` and this file created. No code yet. Next: Phase 0.
