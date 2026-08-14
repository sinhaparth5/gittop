# Changelog

Versions are **CalVer**, `YYYY.MM.PATCH` — the year and zero-padded month of the release, then a
patch number that restarts at `0` each month. `2026.08.0` is the first release of August 2026;
a fix shipped later that month would be `2026.08.1`, and the next release in September would be
`2026.09.0`. There is no major/minor distinction to read into: the month says when, the patch says
how many times since.

The version lives in exactly one place, `project(gittop VERSION ...)` in `CMakeLists.txt`, and
reaches the program from there. `.github/workflows/release.yml` reads the section below whose
heading matches the pushed tag and uses it as the release description, and refuses a tag that does
not match the version CMake was built with — so a heading here that nobody wrote is a release that
does not happen, rather than one that appears empty.

Headings are `## <version> — <date>`. Anything above the first one is this preamble and is never
published.

---

## 2026.08.3 — 2026-08-14

Two bugs that both had the same shape: gittop drew something that looked fine and was not.

### Fixed

- A branch whose upstream was deleted on the remote was drawn as though it had never been pushed.
  It still has an upstream in its config, so "no upstream" invited a `git push -u` that would
  recreate a branch somebody closed on purpose. It now reads `gone`, names the upstream that went
  away, and prints no ahead/behind — counts against a ref that no longer describes anything are not
  facts about the remote. The branch card on the status view had the same gap and gets the same
  treatment.
- The `nerd` glyph set drew blanks. Twenty-nine of its thirty-one icons were empty strings: the
  comments naming them were all still there, the characters were not, and they had been missing
  since the set was introduced. Nothing selects `nerd` automatically, which is why three releases
  shipped it that way. The icons are now written as `\uXXXX` escapes, so a lost one is visible in a
  diff rather than silent.

### Added

- `x` on the Branches view prunes the remote-tracking refs whose branches are gone from the server,
  behind a confirm, and reports which ones went. This is the only thing that can find a stale ref:
  while it still resolves, nothing local can tell it from a healthy one — `git branch -vv` cannot
  either. An ordinary fetch still honours the repository's own `remote.<name>.prune` and is never
  made to prune on gittop's initiative.

---

## 2026.08.2 — 2026-08-14

Documentation. gittop itself is unchanged from 2026.08.1 — same binary, same behaviour — so there
is nothing here to upgrade for unless you want the README.

### Added

- A demo GIF on the README, which is the thing a terminal application is hardest to describe in
  prose. It runs through all ten views, the `/` filter, the help overlay and a theme change.
- `scripts/demo-repo.sh`, which builds the repository the GIF is recorded against: six months of
  history over four branches, two merges, three stashes and a working tree with something in every
  column. It is checked in because a recording nobody can reproduce goes stale the first time the
  UI moves and there is no way to tell that it has. It touches no network — the upstream it pushes
  to is a bare repository beside it.

### Changed

- The README described configuration as a file to edit. Most of it has been a settings page since
  2026.08.0, so the theme, glyph set, panel border, colour depth, animations, startup card and
  compact layout now say so, and the file is described as where they are saved rather than how you
  reach them. A custom palette and the tab set are still the file, and the README now says why.
- Dropped a phase table whose eight rows all read "Done", a note that listed every working feature
  before the section that lists every working feature, and two libgit2 function names that meant
  nothing to anyone deciding whether to install this. Open issues are the roadmap now.
- `demo.tape` recorded against whatever directory you ran it from and used a VHS theme name that no
  longer parses. It also predated the settings and sign-in views, so it never showed them.

---

## 2026.08.1 — 2026-08-13

Packaging. 2026.08.0 was published with release notes and nothing to download — this release is
that gap closed, and it contains no changes to gittop itself.

### Installing

```sh
sudo dpkg -i gittop_2026.08.1_amd64.deb
```

The `.deb` installs on Ubuntu 22.04 and later and Debian 12 and later.
`gittop-2026.08.1-linux-x86_64.tar.gz` is the same build for distributions that do not take debs —
unpack it and put `bin/gittop` on your `PATH`.

On **Windows, run it under WSL** and install the `.deb` there. There is no native Windows build:
the ssh passphrase channel, the browser opener and the `0600` config file permissions are all
POSIX, so a native port is real work rather than a second build target. gittop already recognises
WSL — it uses `wslview` to open a browser and detects Windows Terminal's colour support.

### Added

- A `.deb` and a portable `.tar.gz`, both attached to the release automatically.
- An application icon and a desktop entry, so gittop appears in application menus as well as on
  the command line. The scalable icon is installed for desktops that render SVG and rasters from
  16 to 256 pixels for those that do not.

### Notes

The release binary is built against glibc 2.35 rather than whatever the CI runner happens to
carry. A dynamically linked binary treats its build-time glibc as a floor, so one built on a
current runner demands `GLIBC_2.43` and refuses to start anywhere else — which reads as a corrupt
download rather than an unsupported system. The release now checks this and fails rather than
publishing a binary that would not start.

---

## 2026.08.0 — 2026-08-13

First release, source only — there are no downloads attached to it; build it with CMake, or take
2026.08.1, which attaches a `.deb` and a tarball. gittop is a btop-inspired terminal dashboard for
Git: the local repository first, so it is useful with no network and no account, plus GitHub and
GitLab panels when there is one.

### Ten views

Reached with the number keys, `tab`, or the mouse.

- **Status** — the working tree in two columns: staged, unstaged, untracked and conflicted files
  beside a sidecar carrying the branch, its ahead/behind counts and recent commits. Stage, unstage,
  discard and commit from here.
- **History** — the commit log with a lane gutter, and an activity heatmap over the last thirteen
  weeks.
- **Branches** — local and remote branches with their upstream and ahead/behind.
- **Graph** — a braille area chart over the whole commit timeline, pannable, with selectable
  bucketing.
- **Diff** — the staged or unstaged diff, or a commit's, with per-file jumps.
- **Stashes** — save, apply, pop and drop.
- **Remote** — the repository as the provider describes it: stars, forks, issues, default branch,
  and where the token came from.
- **CI** — GitHub Actions runs and GitLab pipelines for the current branch, with a job drill-down
  on `enter` and a refresh interval that stops itself when anonymous or when the API budget runs
  low, and says which it is doing.
- **Pull requests** — open pull requests and merge requests, with a detail pane.
- **Settings** — one page for connecting an account, switching remote, and everything under
  `[theme]` and `[layout]` that used to mean editing a file.

### Working with a repository

- Stage, unstage, discard, and commit — including during a merge, where every parent is written.
- Fetch, pull and push on a worker thread with a live progress overlay that can be cancelled.
  Pull fast-forwards or refuses; push never forces, asks first, and reports a server-side rejection
  instead of the success libgit2 reports for one.
- Interrupted rebases, merges and cherry-picks are detected and explained, with continue and abort.
- ssh key passphrases are prompted for and served to `ssh` over a unix socket in a private
  directory — never argv, the environment, or a file. gittop honours your own `~/.ssh/config`,
  agent and known_hosts, because it runs the system `ssh` rather than reimplementing it.

### Signing in

`L` opens one overlay with two routes: the OAuth 2.0 device grant (RFC 8628) where the host has a
client application configured, and a guided personal access token where it does not. Tokens are
written to the config at `0600`. An environment variable always wins over a saved token, and a
sign-in that could not be saved keeps working for the session and says it will not survive a
restart.

### Appearance and input

- Eight themes, including `accessible` — a palette whose four file states are separated by
  lightness as well as hue and stay distinct under deuteranopia, protanopia and tritanopia.
  Every status is drawn as a glyph and a letter as well as a colour, so none of them depends on
  colour to be read.
- Three glyph sets — `ascii`, `unicode` and `nerd`. Detection never picks `nerd`: there is no way
  to ask a terminal whether its font has the icons, so it is opt-in.
- Truecolor, 256-colour, 16-colour and monochrome, detected or pinned.
- Reduced motion, which makes every animation snap to its final state rather than removing it —
  a running CI row keeps its glyph and its tint, just a still one.
- Rebindable keys, a configurable tab set, a `/` filter over whichever list is on screen, and
  full mouse support including the wheel and clickable tabs.

### Configuration

`$XDG_CONFIG_HOME/gittop/config.toml`, overridden by `$GITTOP_CONFIG` and then by `--config`.
`--init-config` writes a commented starter file. Per-host settings are table keys, so a self-hosted
GitLab or GitHub Enterprise instance is configured rather than patched. Every rejected line is
reported by name.

### Known limits

- Built and tested on Linux.
- No OAuth client application is compiled in, so signing in uses the personal access token route
  unless a `client_id` is configured for the host. This is a packaging decision, not a missing
  feature — the token route is a first-class path.
- Saving from the settings page regenerates the config file, which drops the comments out of a
  hand-written one. The starter file says so.
- There is no test suite yet; verification is done by driving the TUI and reading rendered frames.
