# gittop — Progress

A btop-inspired terminal dashboard for Git: local repo state that always works offline, plus
remote-aware CI/pipeline and PR/MR panels for GitHub and GitLab.

**Status:** Phase 0 — not started
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
| Auth | Personal Access Tokens | Env var first, config file second — see "Auth" below |
| Build | CMake + FetchContent | |
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

### Phase 0 — Project setup
- [ ] `CMakeLists.txt` with C++20, FetchContent for FTXUI
- [ ] Hello-window: FTXUI screen that renders and exits cleanly on `q`
- [ ] libgit2 wired in and linking
- [ ] Detect whether cwd is inside a Git repo; friendly message if not
- [ ] `.gitignore` already covers CMake output — verify `build/` is ignored

### Phase 1 — Local status dashboard
- [ ] Read status: staged / unstaged / untracked / conflicted
- [ ] Status boxes with counts and btop-style progress bars
- [ ] Scrollable file list with selection
- [ ] Stage / unstage selected file
- [ ] Discard changes (with confirmation — destructive)
- [ ] Commit: message input popup → `git_commit_create`
- [ ] Keybinding help overlay (`?`)
- [ ] Minimal theme token layer — semantic names only, one hardcoded palette behind them

### Phase 2 — History & visualization
- [ ] Commit log panel (walk revwalk, paginated)
- [ ] Commit graph rendering (braille/box-drawing lanes)
- [ ] Activity heatmap — commits over last 30/90 days
- [ ] Branch list with ahead/behind vs upstream
- [ ] Responsive layout: panels reflow at narrow widths

### Phase 3 — Remote foundation
- [ ] Config file (location TBD — see open questions) with load/save
- [ ] Token resolution: env var → config file → unauthenticated
- [ ] Parse remote URL → detect GitHub vs GitLab vs self-hosted vs unknown
- [ ] libcurl client with timeout, retry, and error surfacing
- [ ] Async fetch layer — **UI thread never blocks on network**
- [ ] Fetch + display basic repo info

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

**Async is a Phase 3 problem but a Phase 0 design constraint.** FTXUI's event loop needs
`ScreenInteractive::PostEvent` (or a custom loop) to be woken from a worker thread. If Phase 1's
UI is written assuming synchronous data, Phase 3 becomes a rewrite. Sketch the data-refresh
seam before writing the first panel.

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

- Config file location — `~/.config/gittop/config.toml` (XDG) vs `~/.gittoprc`? Format: TOML, JSON, or INI?
- Self-hosted GitLab / GitHub Enterprise: support custom base URLs from the start, or defer?
- Minimum terminal size to support, and behavior below it
- Test strategy: unit tests against fixture repos created at test time? Which framework (Catch2 / doctest / GoogleTest)?
- Target platforms — Linux only initially, or macOS too?

---

## Work log

Newest first. One line per session: what changed, what's next.

- **2026-08-09** — Added Phase 7 (visual design pass); Phase 6 renamed to "Power features" to
  keep functional work distinct from visual work. Theme token layer pulled forward into Phase 1
  so Phase 7 stays a restyle.
- **2026-08-09** — Repo initialized (README, GPL-3.0 LICENSE, CMake `.gitignore`). Plan locked,
  `CLAUDE.md` and this file created. No code yet. Next: Phase 0.
