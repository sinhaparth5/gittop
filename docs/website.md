# gittop

A btop-style terminal dashboard for Git and CI.

gittop puts your working tree, commit history, CI runs and pull requests on one screen in the
terminal. Most tools make you pick one: a Git TUI shows the repository but knows nothing about your
pipeline, and the GitHub web UI shows the pipeline but takes you out of the terminal. gittop does
both.

The local views read the repository directly through libgit2, so they work with no network and
never shell out to `git`. The remote views talk to the GitHub and GitLab APIs. When there is no
token, no remote or no connection, gittop falls back to the local views.

## Install

### Linux

Linux x86_64 with glibc 2.35 or newer (Ubuntu 22.04, Debian 12 or later):

```bash
curl -fsSL https://raw.githubusercontent.com/sinhaparth5/gittop/master/install.sh | sh
```

This installs `gittop` to `~/.local/bin` without sudo and checks the download against the release's
`SHA256SUMS` file. If the checksum does not match, it installs nothing. Run the same command again
to update; if you already have the latest version, it says so and does nothing.

- `GITTOP_PREFIX=/some/dir` installs somewhere else.
- `GITTOP_VERSION=2026.10.1` pins a specific release.
- To uninstall, delete `~/.local/bin/gittop`.

You can also download a package from the
[releases page](https://github.com/sinhaparth5/gittop/releases):

```bash
sudo apt install ./gittop_*_amd64.deb    # Ubuntu 22.04+, Debian 12+
tar xzf gittop-*-linux-x86_64.tar.gz     # any other distro: put bin/gittop on your PATH
```

Both packages include an icon and a desktop entry, so gittop also appears in your application menu.

### Windows

gittop has a native `gittop.exe`, available in two forms:

- `gittop-<version>-windows-x86_64.exe` is an installer that can add gittop to your `PATH`.
- `gittop-<version>-windows-x86_64.zip` unpacks and runs with no administrator rights.

The installer does not add gittop to `PATH` by default. Choose "for all users" or "for the current
user" on that page if you want to type `gittop` in a terminal, then open a new terminal. If your
`PATH` is already too long, the installer leaves it alone and tells you which folder to add yourself.

It is a single self-contained binary with no Visual C++ redistributable or OpenSSL to install. HTTPS
uses the Windows certificate store, and ssh remotes use the `ssh.exe` that ships with Windows, so
your `~/.ssh/config`, agent and `known_hosts` work as usual.

Use Windows Terminal for full 24-bit colour. The old console window only shows sixteen colours.
gittop also runs in WSL: install the Linux package inside it.

### macOS

Coming soon.

### Build from source

You need a C++20 compiler, CMake 3.24 or newer, zlib and libcurl. CMake downloads and builds FTXUI,
libgit2 and nlohmann/json for you, so the first configure takes a few minutes.

```bash
sudo apt install libcurl4-openssl-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/gittop            # the repository you are standing in
./build/gittop /some/repo # or one you name
```

On Windows, with Visual Studio 2022 or newer:

```
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
cmake --build build --config Release
```

## Views

There are ten tabs. Press `1` to `9` and `0` to jump to one, or `tab` to cycle through them.

### Status

- Cards for staged, unstaged, untracked and conflicted counts, with bars that animate to their new
  value when something changes.
- A file list grouped by state. Directories are dimmed and filenames are bright, so long lists are
  easy to scan.
- Stage and unstage a file with `space`, `s` and `u`, or stage everything with `a`.
- Discard a change with `d`, behind a confirm. Tracked files are restored from the index and
  untracked files are deleted. The confirm says which will happen.
- Commit with `c`, amend the last commit with `A`, or write the message in your `$EDITOR` with `C`.
  Amending a commit you already pushed asks first.
- Each status is shown with a glyph and a letter as well as a colour, so you can read it without
  telling green from amber.

### History

- The commit log with a lane graph, branch and tag badges, author and age.
- A 91-day activity heatmap.
- Interactive rebase straight from the list: `s` squash, `f` fixup, `d` drop, `w` reword, and `J`
  and `K` to move a commit down or up. One confirm names the rewrite and how many of those commits
  are already pushed.
- Cherry-pick: mark commits with `c`, then paste them with `V`, oldest first, keeping their authors.
- Revert a commit with `t`.
- Tag a commit with `T`, lightweight or annotated, and then push the tag.

### Branches

- Local branches with ahead and behind counts against their upstream, with the current branch first.
- Remote branches that nothing tracks yet are listed as `remote only`.
- `enter` checks out a branch. For a remote-only branch it creates a local one that tracks it. If
  an uncommitted change is in the way, it stops instead of overwriting it.
- `n` creates a branch, `R` renames one and `D` deletes one. Deleting warns you when a branch is not
  merged and asks separately before deleting it on the remote.
- A branch whose upstream was deleted shows as `gone`. `x` prunes stale remote-tracking refs.

### Graph

- A chart of commits over the whole history.
- `h` and `l` pan through time, and `g` and `G` jump to the oldest and newest.
- `d`, `w` and `m` switch between days, weeks and months.
- Top authors, a weekday breakdown, and commits by hour of day in each author's own timezone.

### Diff

- The unstaged diff, the staged diff, or any commit's diff, with old and new line numbers.
- `enter` on a file in Status opens its diff. `enter` on a commit in History opens that commit's.
- `s` switches between unstaged and staged, and `[` and `]` jump between files.
- `space` stages or unstages a single hunk or line.
- Added and removed line counts per file and in total.
- New files show their contents, binary files are named, and renames are drawn as renames.
- Diffs stop at 20,000 lines and say where they stopped.

### Blame

- `b` on a file in Status or Diff opens blame for that file, following renames.
- Each block of lines from one commit shows its id, author and age once.
- `enter` opens the commit for that line, and `/` filters by author or summary.

### Stashes

- `S` stashes the whole working tree, including untracked files.
- Each entry shows its index, the branch it came from, its message and its age.
- `a` applies, `p` pops and `d` drops. Pop and drop ask first.
- Apply and pop stop instead of overwriting uncommitted changes, and name the file in the way.

### Remote

- Repository details from GitHub or GitLab: description, default branch, visibility and last push.
- Stars, forks, open issues and watchers.
- Your remaining API budget as a bar, so you can see a rate limit coming.
- Works without a token on public repositories.
- Detects self-hosted GitHub Enterprise and GitLab from the hostname. You can also name a host in
  the config.
- `R` switches between remotes when a repository has more than one.

### CI

- GitHub Actions runs and GitLab CI pipelines for your current branch.
- `b` picks another branch, a tag, or all refs. This matters if your workflows run on tags, because
  those runs belong to the tag and not to the branch you are on.
- The header always names the ref being shown.
- Passed, failed, running, queued, manual, cancelled and skipped each have their own glyph, word
  and colour.
- Run duration, which counts up while a run is going, and when it started.
- `enter` opens a run's jobs, grouped into stages on GitLab.
- Refreshes on a timer while the view is open and shows when the next refresh is due. Polling only
  runs with a token and pauses when less than a fifth of your API budget is left.

### Pull requests

- Open GitHub pull requests and GitLab merge requests in one list.
- Draft, open, merged and closed states.
- `enter` shows branches, author, labels, reviewers and the link.
- GitLab's mergeable, conflicting and blocked states, where GitLab reports them.
- The pull request for your current branch is marked and sorted to the top.
- `n` opens a new pull request without leaving the terminal. It targets the default branch and uses
  your newest commit as the title, and you can change both. It asks before sending.
- If your branch was never pushed, it tells you and points to the push key. If you have commits you
  have not pushed, it warns you but still opens it.
- If the provider rejects it, you see the provider's own message and your text is still in the form.

### Settings

- Theme, glyph set, panel border, colour depth, animations, compact layout and the startup splash.
- Sign in to GitHub or GitLab, or sign out.
- Pick which remote the network views use.
- A sidebar showing the repository, its remotes (with any credentials removed from URLs) and where
  the config file is.
- Changes apply and save immediately. gittop edits your config file in place, so your comments,
  ordering and spacing stay as you wrote them.

### Worktrees and submodules

- `W` lists every worktree with its branch and number of changed files, and every submodule with the
  commit HEAD records and the one checked out.
- `enter` switches gittop to a worktree.
- `n` checks out a branch in a new worktree, creating the branch at HEAD if needed.
- `D` removes a worktree after a confirm. It refuses one with changes and prunes one whose folder is
  already gone.
- `u` clones or updates the selected submodule.
- This view has no tab by default. Add `workspace` to `[layout] views` to give it one.

## Everywhere

- `/` filters whichever list is on screen and counts matches as you type. `enter` keeps the filter
  and `esc` clears it.
- `z` undoes the last change to HEAD, including a whole rebase, and `Z` opens the reflog. From the
  reflog, `enter` resets to an entry and `n` makes a branch from it. Both ask first and refuse on a
  dirty working tree.
- When a merge, rebase, cherry-pick, revert or bisect is left unfinished, a banner says which, and
  how far along a rebase is. `o` lets you continue or abort, each behind a confirm.
- `B` rebases the current branch onto its upstream. It refuses on a dirty working tree instead of
  stashing for you.
- Committing during a merge records every parent and clears the merge state.
- gittop refreshes by itself when files in the repository change. Ignored files are not watched.
- The mouse works: scroll with the wheel, click to select or switch tabs, and click a selected row
  again to open it.
- `ctrl-v` pastes into any input box. A pasted newline becomes a space, so pasting never submits a
  box by accident.
- The layout adapts to the terminal size. gittop needs at least 80×24 and shows the current and
  required size when the window is smaller.

## Push and pull

`f` fetches, `p` pulls and `P` pushes. Each runs in the background with a progress bar, and `esc`
cancels.

Pull only fast-forwards. If your branch has diverged, gittop fetches, tells you, and points you at
`B` to rebase. If a fast-forward would overwrite an uncommitted change, it stops and names the file.

Push never forces, and asks before it runs. A branch with no upstream gets one, like
`git push -u`.

Over https, your token is the password. Over ssh, gittop uses your system `ssh`, so your
`~/.ssh/config`, agent, keys and `known_hosts` all work. If your key has a passphrase and no agent
holds it, gittop asks for it once per session.

## Signing in and tokens

Public repositories need no token. GitHub allows 60 requests an hour without one, and 5000 with one.
A token also unlocks private repositories.

Press `L`, or use the row on the Settings page, to sign in:

- If the host has an OAuth app configured, gittop shows a code, opens your browser and waits. This
  also works over ssh.
- Otherwise it opens the provider's token page with the name and scopes filled in, and you paste
  the token back.

The token is saved to your config file, readable only by you. If the file cannot be secured, the
save fails rather than leaving the token readable. You can also use an environment variable, which
always wins:

```bash
export GITHUB_TOKEN=ghp_...      # or GH_TOKEN, or GITTOP_TOKEN for either provider
export GITLAB_TOKEN=glpat-...    # or CI_JOB_TOKEN
```

gittop never prints a token, masked or otherwise. It only names the variable or file it came from.

## Themes and appearance

Nine themes: `mainframe` (the default), `default`, `catppuccin`, `gruvbox`, `nord`, `tokyo-night`,
`dracula`, `daylight` and `accessible`. Press `t` to cycle, or pick one in Settings.

`mainframe` is white on black with faint mint, amber and slate tints, and red only for conflicts
and danger. `daylight` is a light theme. `accessible` puts the four status colours on a blue and
amber scale that stays distinct for every common type of colour blindness.

You can write your own palette under `[theme.colors]` in the config. Anything you leave out keeps
its default.

gittop picks the colour depth for your terminal: truecolor, 256 colours, 16 colours, or none under
`NO_COLOR`. You can override it in Settings.

There are three glyph sets: `ascii`, `unicode` and `nerd`. `auto` chooses `ascii` or `unicode` for
your terminal and never `nerd`, since it cannot tell whether your font has the icons. Set
`logos = true` to show the real GitHub and GitLab logos from a Nerd Font while keeping the rest of
the interface on your chosen set.

Text is measured in terminal cells, so Japanese commit messages and emoji in filenames are cut
cleanly without breaking panel borders.

`animations = false` turns off movement. Bars, spinners and fades jump to their final state, and
nothing on screen is lost. With animations on, frames are capped at 30 per second, and an idle
dashboard draws nothing.

## Configuration

```bash
gittop --init-config    # write a commented starter config
gittop --config-path    # print where gittop looks for it
gittop --config <file>  # use a different file
```

The config lives at `~/.config/gittop/config.toml` on Linux and `%APPDATA%\gittop\config.toml` on
Windows.

```toml
[theme]
name = "mainframe"
icons = "auto"        # auto, ascii, unicode, nerd
logos = false         # real GitHub and GitLab logos; needs a Nerd Font
border = "light"      # light, rounded, heavy, double
animations = true
splash = true

[layout]
views = "status diff history branches stashes"
start_view = "status"
compact = false

[general]
auto_refresh = true
```

`[layout] views` chooses which tabs exist and in what order. The number keys follow the order, so
`2` always goes to the second tab shown. `compact` forces the narrow layout at any width.

### Key bindings

Every key can be rebound under `[keys]` by action name. The footer and the help screen (`?`) always
show the keys currently in use.

### Custom commands

Bind your own commands to keys:

```toml
[[commands]]
key = "ctrl-f"
scope = "branches"          # optional: only on this view
run = ["git", "fetch", "{remote}", "{branch}"]
confirm = true              # optional: ask first and show the full command
```

`run` is a list of arguments and never goes through a shell. These placeholders are filled in from
what you have selected:

- `{branch}`: the selected branch on Branches, otherwise the current branch.
- `{commit}`: the selected commit on History, otherwise `HEAD`.
- `{file}`: the file under the cursor on Status, Diff or Blame.
- `{remote}`: the current remote.

Anything else in braces, such as `@{u}`, is passed through to git. Commands run in the repository's
top folder in the background, the last line of output appears on screen, and every view reloads
afterwards. A custom command's key overrides a built-in one.

## Keys

| Key                   | Action                                                    |
| --------------------- | --------------------------------------------------------- |
| `1` to `9`, `0`       | Jump to a tab                                             |
| `tab` / `shift-tab`   | Next / previous view                                      |
| `j` `k` or arrows     | Move                                                      |
| `g` `G`               | First / last                                              |
| `ctrl-u` `ctrl-d`     | Page up / down                                            |
| `/`                   | Filter                                                    |
| `enter`               | Open: diff, blame line, CI jobs, PR details, checkout     |
| `space`               | Status: stage or unstage. Diff: stage a hunk or line      |
| `s` `u` `a`           | Status: stage / unstage / stage all                       |
| `d`                   | Status: discard                                           |
| `c` `A` `C`           | Status: commit / amend / commit in `$EDITOR`              |
| `b`                   | Blame the file. CI view: pick a ref                       |
| `s` `[` `]`           | Diff: switch side / previous file / next file             |
| `s` `f` `d` `w`       | History: squash / fixup / drop / reword                   |
| `J` `K`               | History: move commit down / up                            |
| `c` `V` `t`           | History: mark / cherry-pick marked / revert               |
| `T`                   | History: tag                                              |
| `n` `R` `D`           | Branches: new / rename / delete                           |
| `x`                   | Branches: prune                                           |
| `h` `l` `d` `w` `m`   | Graph: pan / day / week / month                           |
| `S`                   | Stash                                                     |
| `a` `p` `d`           | Stashes: apply / pop / drop                               |
| `n`                   | Pull requests: open one                                   |
| `W`                   | Worktrees and submodules                                  |
| `n` `D` `u`           | Worktrees: add / remove / update submodule                |
| `B`                   | Rebase onto upstream                                      |
| `o`                   | Continue or abort an unfinished operation                 |
| `z` `Z`               | Undo / reflog                                             |
| `f` `p` `P`           | Fetch / pull / push                                       |
| `R`                   | Next remote                                               |
| `L`                   | Sign in                                                   |
| `t`                   | Next theme                                                |
| `r`                   | Reload                                                    |
| `?`                   | Help                                                      |
| `q`                   | Quit                                                      |

Some keys mean different things on different views. `s` stages on Status and squashes on History,
for example. Which one applies depends on the view you are on.

## Safety

- Pull only fast-forwards and push never forces.
- Anything that can destroy work you cannot get back, or that other people can see, asks first:
  discard, drop, prune, deleting a branch or worktree, aborting a merge, rewriting history, reset,
  push and opening a pull request.
- Checkout, stash apply and pop, pull, and undo all stop instead of overwriting uncommitted changes.
- Custom commands, browser links and clipboard reads never go through a shell.
- Tokens and passphrases are never printed or logged, and remote URLs have credentials removed
  before they are shown.

## Coming later

- macOS builds.
- Merging, so a diverged pull has an option besides rebasing.
- More than one page of CI and pull request history.
- Merging pull requests.
- Picking pull request branches from a list.

## Built with

C++20, [FTXUI](https://github.com/ArthurSonzogni/FTXUI), [libgit2](https://libgit2.org/),
[libcurl](https://curl.se/libcurl/) and [nlohmann/json](https://github.com/nlohmann/json).

## License

GPL-3.0. Source on [GitHub](https://github.com/sinhaparth5/gittop). Changes for each release are in
[CHANGELOG.md](https://github.com/sinhaparth5/gittop/blob/master/CHANGELOG.md).
