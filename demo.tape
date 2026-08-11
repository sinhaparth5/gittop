# A VHS script for the README's demo GIF: https://github.com/charmbracelet/vhs
#
#   vhs demo.tape
#
# Checked in rather than the GIF alone, because a recording nobody can reproduce
# goes stale the first time the UI moves and there is no way to tell that it has.
# Run it from a repository with some history in it — the graph and the heatmap
# are the frames worth having and both are empty in a fresh tree.
#
# Nothing here touches the network. The remote, CI and pull tabs are deliberately
# not visited: they would either sit on a spinner or put somebody's repository
# and rate-limit budget into a public image.

Output demo.gif

Set Shell        "bash"
Set FontSize     15
Set Width        1400
Set Height       860
Set Padding      18
Set Theme        "Tokyo Night"
Set TypingSpeed  55ms

Hide
Type "./build/gittop" Enter
Sleep 500ms
Show

# The splash, then the status view it is sitting in front of.
Sleep 2s

# Stage something and watch the bars ease across.
Down Sleep 400ms
Down Sleep 400ms
Space Sleep 1200ms

# History: the log with its lane gutter, and the activity heatmap above it.
Type "2" Sleep 2s
Down Sleep 300ms
Down Sleep 300ms
Down Sleep 800ms

# The commit under the cursor, as a diff.
Enter Sleep 2s
Down Down Down Down Sleep 1s

# Branches, with the per-branch velocity sparklines.
Type "3" Sleep 2s

# The graph: braille area chart, pannable, three bucket sizes.
Type "4" Sleep 1500ms
Type "w" Sleep 1500ms
Type "m" Sleep 1500ms

# The filter, live, with its match count.
Type "2" Sleep 800ms
Type "/" Sleep 500ms
Type "fix" Sleep 1500ms
Escape Sleep 800ms

# The help overlay, which is where the theme and the colour depth are printed.
Type "?" Sleep 2500ms
Escape Sleep 500ms

# Themes.
Type "t" Sleep 1200ms
Type "t" Sleep 1200ms
Type "t" Sleep 1200ms

Type "q"
Sleep 1s
