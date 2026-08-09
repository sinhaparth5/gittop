# gittop — Progress

A btop-inspired terminal dashboard for Git: local repo state that always works offline, plus
remote-aware CI/pipeline and PR/MR panels for GitHub and GitLab.

**Status:** Phases 0–3 complete and running. Next: Phase 4 (pipelines & CI).
**Started:** 2026-08-09
**Last updated:** 2026-08-09

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

### Phase 4 — Pipelines & CI
- [ ] GitHub: list workflow runs for current branch
- [ ] GitLab: list pipelines for current branch
- [ ] Normalize both into `model::Pipeline`
- [ ] Status indicators: success / failed / running / pending / cancelled
- [ ] Job-level drill-down
- [ ] Live refresh on interval, respecting rate limits
- [ ] Rate-limit display (GitHub exposes remaining/reset headers)

### Phase 5 — Full remote features
- [ ] Pull Requests / Merge Requests panel
- [ ] Push / pull with progress and auth handling
- [ ] Multiple remotes
- [ ] Themes
- [ ] Mouse support
- [ ] Configurable keybindings

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

**Color & themes**
- [ ] Truecolor palette with graceful 256-color and 16-color degradation
- [ ] Terminal capability detection (`COLORTERM`, `TERM`) driving the fallback
- [ ] Built-in themes: default (btop-flavored), Catppuccin, Gruvbox, Nord, Tokyo Night, Dracula
- [ ] At least one light theme that genuinely works, not an inverted dark theme
- [ ] User themes loadable from config
- [ ] `NO_COLOR` env var respected

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
deps. Decide early; retrofitting is painful.

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
