# A VHS script for the README's demo GIF: https://github.com/charmbracelet/vhs
#
#   scripts/demo-repo.sh demo-repo     # once, to build what it records against
#   vhs demo.tape
#
# Checked in rather than the GIF alone, because a recording nobody can reproduce
# goes stale the first time the UI moves and there is no way to tell that it has.
#
# It records against `demo-repo/`, which scripts/demo-repo.sh generates: six
# months of history across four branches, with two merges, a stash and a working
# tree that has something in all of staged, unstaged and untracked. gittop's own
# repository is the wrong subject — a couple of dozen commits over a few days
# leaves the graph a spike and the heatmap one cluster at the right edge, and
# those are two of the frames worth having.
#
# Nothing here touches the network. The remote, CI and pull tabs are deliberately
# not visited: they would either sit on a spinner or put somebody's repository
# and rate-limit budget into a public image.

Output assets/demo.gif

Set Shell        "bash"
Set FontFamily   "DejaVu Sans Mono"
Set FontSize     18
Set LineHeight   1.1
Set Width        1600
Set Height       900
Set Padding      20
Set Theme        "TokyoNight"
Set TypingSpeed  55ms
# Thirty frames keep the bars and splash fluid without paying the default 50
# fps cost. The oversized canvas is intentionally downscaled by the README,
# giving its text and box drawing cleaner edges at the displayed 900px width.
Set Framerate    30

Hide
# Start from a deterministic, disposable config instead of reading or changing
# the recorder's real settings. Clear token variables so account state cannot
# leak into the public GIF, and tell the ttyd session it supports truecolor.
Type "rm -f demo-repo/.gittop-demo.toml" Enter
Sleep 200ms
Type "env -u NO_COLOR -u GITTOP_TOKEN -u GITHUB_TOKEN -u GH_TOKEN -u GITLAB_TOKEN -u CI_JOB_TOKEN TERM=xterm-256color COLORTERM=truecolor ./build/gittop --config demo-repo/.gittop-demo.toml demo-repo" Enter
Sleep 300ms
Show

# The splash holds for 1.6s (kSplashSeconds) and only runs at all when stdout is
# a terminal — which it is under ttyd, so it is in the capture. Then the status
# view it eases into.
Sleep 2800ms

# Stage something and watch the bars ease across.
Down Sleep 400ms
Down Sleep 400ms
Space Sleep 1400ms

# History: the log with its lane gutter, and the activity heatmap above it.
Type "2" Sleep 2s
Down Sleep 300ms
Down Sleep 300ms
Down Sleep 800ms

# The commit under the cursor, as a diff.
Enter Sleep 2s
Down Down Down Down Sleep 1200ms

# Branches, with the per-branch velocity sparklines. Two of these were merged
# and one is still ahead, which is what makes the column worth looking at.
Type "3" Sleep 2500ms

# The graph: braille area chart, pannable, three bucket sizes.
Type "4" Sleep 1500ms
Type "w" Sleep 1500ms
Type "m" Sleep 1500ms

# Stashes.
Type "6" Sleep 2s

# The filter, live, with its match count.
Type "2" Sleep 800ms
Type "/" Sleep 500ms
Type "fix" Sleep 1800ms
Escape Sleep 800ms

# The help overlay, which is where the theme and the colour depth are printed.
Type "?" Sleep 2500ms
Escape Sleep 500ms

# Finish on Settings and change the theme in place. The theme is the third row:
# two downs select it, then enter performs the row's advertised "next theme"
# action. Keeping the cursor here makes each palette change clear and causal.
Type "0" Sleep 1800ms
Down Sleep 400ms
Down Sleep 800ms
Enter Sleep 1300ms
Enter Sleep 1300ms
Enter Sleep 1800ms

Type "q"
Sleep 1s
