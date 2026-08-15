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

## 2026.08.8 — 2026-08-15

You can open a pull request without leaving.

### Added

- **`n` on the pull requests view opens one.** Listing them but not being able to make one meant
  leaving for a browser or the provider's CLI at exactly the moment the work was finished, which is
  the one step that broke the premise. The form has four fields, which is the whole of what the two
  providers agree on: the branch it comes from, the branch it goes into, a title and a description.

  The branch you are on and the repository's default branch are already filled in, and the title
  starts as your newest commit. `tab` moves between the boxes, `enter` opens it, `esc` leaves.
  Both branches are free text — picking them from a list needs remote branches gittop cannot see
  yet, and waiting for that would have been waiting for a different feature.

- **It asks first, the way push does.** Opening a pull request and pushing are the two things gittop
  does that other people can see, and that is the whole rule the confirms implement. The pane shows
  which host and repository, which branch goes into which, and the title, before anything is sent.

- **It checks what it already knows before spending a request on being refused.** A branch that was
  never pushed cannot be merged from: gittop says so, names the push key, and does not send. A
  branch that is merely ahead of its upstream is a different piece of news and gets a different
  answer — it warns that the unpushed commits will not be in it, and opens anyway, because they are
  not the same problem and refusing the second would be wrong.

  Both facts come out of the status read that has already happened, so neither costs anything.

- **A refusal gives you the provider's own sentence and your draft back.** "No commits between
  master and feature" is something to act on; "unexpected response 422" is not. GitHub and GitLab
  bury that sentence in five different shapes between them and gittop digs it out of all of them.
  Nothing you typed is lost — the form comes back with every field still in it, which is also true
  of answering `n` to the confirm.

- Signing in already asked for write access, because the same token authenticates an https push, so
  nobody has to re-authorize for this.

### Fixed

- Nothing. This release is the feature above.

---

## 2026.08.7 — 2026-08-15

gittop runs on Windows.

### Added

- **A native `gittop.exe`.** Two downloads on the release page: an installer that offers to put
  gittop on your `PATH`, and a zip you can unpack and run without an administrator. It is one
  self-contained file — no Visual C++ redistributable, no OpenSSL, nothing to install beside it.

  WSL was the previous answer and still works, but it was an answer about where to run Linux rather
  than about Windows.

  What that took, in case any of it matters to you:

  - HTTPS goes through **WinHTTP and Schannel**, so gittop trusts the certificates Windows trusts
    and there is no CA bundle shipped alongside it that could go stale.
  - ssh remotes run the **`ssh.exe` that ships with Windows**, which means your `~/.ssh/config`,
    your agent and your `known_hosts` work exactly as they do from any other terminal — the same
    property the Linux build has, and for the same reason.
  - The config lives at `%APPDATA%\gittop\config.toml`. `gittop --config-path` prints it.
  - It is secured with an **owner-only ACL** rather than `0600`, and the ACL is written *protected*
    so it does not silently inherit whatever the parent folder was granting. A write that cannot
    secure the file now fails and says so instead of leaving a token where other accounts can read
    it — on every platform, not only this one.
  - **ctrl-v** reads the Windows clipboard directly rather than looking for `wl-paste` or `xclip`,
    which do not exist there.
  - gittop asks the console how much colour it can render, because a Windows console sets neither
    `TERM` nor `COLORTERM` — the two variables every other terminal answers that with. Without it
    the dashboard came out monochrome on a machine perfectly capable of 24-bit colour. Use Windows
    Terminal if you can; a legacy `conhost` window predates 24-bit support and gets sixteen colours.

  Known limitation: ssh key **passphrase prompting** is implemented on Windows but unverified there.
  If your key is passphrase-protected and not loaded into an agent, a push may fail rather than
  asking. Keys in `ssh-agent`, and unencrypted keys, are unaffected.

### Changed

- `scripts/build-windows.sh` builds `gittop.exe` from a Linux checkout and runs it under Wine, using
  only docker. This project has no test target and CI only runs on a tag, so without it the first
  thing to notice a broken Windows build would have been a release. It found two real portability
  bugs the first time it ran.

---

## 2026.08.6 — 2026-08-15

Two ways gittop was losing things you gave it: settings you changed, and text you pasted.

### Changed

- Settings save themselves. Every toggle on the settings page, and `t` from anywhere else, is
  written to the config the moment it takes effect — there is no save step. The row that used to be
  the only way to write the file is still there, but it is now the retry for a write that failed
  rather than the thing standing between a setting and it surviving a restart.
- Writing the config **edits the file rather than replacing it**. This is what made the above safe,
  and it is the more important half: the writer used to regenerate the whole file from the settings
  gittop was holding, so every save flattened a hand-written config into a machine-written one and
  the comments went with it. Your comments, your ordering and your spacing now all survive, an
  unchanged value keeps its line byte for byte, and a new setting is written *inside* the table it
  belongs to rather than appended after the last one. A line gittop cannot parse is left exactly
  where it is instead of being dropped — it could never read those, which is not the same as you
  having deleted them.

  Signing in benefits from the same change: writing a token now touches the token's line and
  nothing else. The note in the starter config warning that a save would eat your comments is gone,
  because it is no longer true.

### Added

- **ctrl-v pastes into any input box** — the commit message, the sign-in token, the `/` filter and
  the ssh passphrase. No terminal sends the clipboard on ctrl-v: paste is ctrl-shift-v, and plain
  ctrl-v arrives as a byte every input box on earth ignores, so for anyone whose habits come from a
  GUI editor paste simply did nothing and said nothing. gittop now reads the clipboard itself, via
  `wl-paste`, `xclip` or `xsel` — whichever the session has. Text lands where the cursor is, not on
  the end. If none of the three is installed, it says so and names them rather than failing quietly.

### Fixed

- **A pasted newline submitted the box it landed in, and in the commit box that meant it committed.**
  Every input in gittop is a single line, so the newline in the middle of a two-line clipboard
  reached the box as `enter`. Pasting two lines into the commit message produced a real commit,
  titled with the two lines run together, from one paste and no confirmation — the worst version of
  this, because it is the one that writes to the repository. In the `/` filter the same newline
  closed the box halfway through the paste.

  gittop now asks the terminal to mark pastes and treats what arrives between the marks as text
  rather than as keystrokes, so a newline inside a paste becomes a space and the box waits for you.
  Two joined lines still read as two words.
- A pasted personal access token is trimmed before it is used. Copying one from a web page usually
  brings a newline along, and an untrimmed token fails with a 401 that reads as "this token is
  wrong" rather than as "there is whitespace on the end of it".

---

## 2026.08.5 — 2026-08-15

One thing you can see, and one you could only have hit at the worst possible moment.

### Added

- The CI header draws the GitHub Actions mark rather than the GitHub one. It prints the words
  "GitHub Actions" and was putting the plain octocat beside them, which made it the only place
  gittop showed one company's mark next to a different product's name. Actions is separately
  branded and has a logo of its own, so it gets it. There is no GitLab counterpart on purpose:
  GitLab CI is branded as GitLab, so the tanuki is already the right mark there and a second
  invented shape would be the only wrong answer available. This rides the existing `theme.logos`
  setting — nothing new to turn on, and still ignored under `icons = "ascii"`, where the terminal
  has said it cannot carry the bytes.

### Fixed

- gittop could hang on quit instead of exiting, after an ssh transfer that had asked for a key
  passphrase. Shutting the passphrase helper down means waking its listener thread with a single
  byte down a pipe, and that write was issued without checking whether it landed: a signal arriving
  at the wrong instant leaves the thread parked waiting for a byte that was never delivered, and the
  quit path waits on that thread forever. The write is now retried the way every other write in that
  file already was, and the pipe is closed before the wait rather than after it, so the thread wakes
  on the hangup even in the case where the write failed anyway. Narrow to reach and total when
  reached — the only way out was to kill the process.

---

## 2026.08.4 — 2026-08-14

Both changes are the same complaint from two directions: a panel that was showing you less than it
appeared to, without saying so.

### Added

- `b` on the CI view picks which ref the run list is filtered to — the checked-out branch, every
  ref, or any branch or tag in the repository. It was pinned to whatever HEAD was on, which on a
  repository whose workflow triggers on tags means every run it has is attributed to a tag and the
  panel is permanently empty. An empty run list and a repository with no CI at all looked identical
  and there was no way to tell them apart without leaving the app. The picker offers tags because
  that is where those runs are: a tag-triggered run carries the tag name in the same field a branch
  would be in, on both providers, so one filter covers both. The header now always names what is
  being filtered on, and tells an "all refs" you chose apart from a detached HEAD falling into it —
  the same request, two different facts. Rebindable as `ci_ref` under `[keys]`.
- `theme.logos` draws the GitHub and GitLab marks as their real logos from a Nerd Font, and there is
  a *provider logos* row on the settings page that toggles it. It is deliberately separate from
  `theme.icons`: those two marks are the only glyphs gittop draws that are logos, and no arrangement
  of geometric shapes is the octocat or the tanuki, whereas the unicode `✓` is a perfectly good
  check. So this turns on the part of the nerd set worth having without committing the rest of the
  interface to a patched font. Off by default, for the same reason nothing selects `nerd`
  automatically, and ignored under `icons = "ascii"`.
- The settings page's REMOTES card marks each remote with its provider, so an origin on one host and
  a mirror on the other are one glyph apart rather than two URLs to read.

### Fixed

- A long branch name pushed the CI header off the end of the row. FTXUI clips at the cell and says
  nothing, so the header read `0 runnext refresh in` with no sign anything was missing. The header
  chip, the empty state and the picker rows are now measured in cells and truncated, which is what
  the rest of gittop already did.

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
