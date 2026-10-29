# Design: merge and the conflict resolver

Status: implemented on the `conflict-resolver` branch. This records why it is shaped the way it is.

## Requirements

Functional:

- Merge a branch into HEAD from the Branches view: fast-forward, clean merge commit, or a stop.
- Resolve each conflicted block by taking ours, theirs or both.
- Resolve a whole file by taking one side, including binary files and modify/delete conflicts.
- Hand a file to the user's editor (vim and nvim at the conflict's line), then pick up the result.
- Work the same whether a merge, cherry-pick, revert or rebase stopped.

Non-functional:

- Never write a block the user was not looking at.
- Never stage a file that still has a conflict in it.
- No new dependency, no network, no worker thread.

Out of scope: a three-way merge editor, word-level conflicts, `conflict-marker-size` other than 7,
`+line` jumps for editors other than vi, vim and nvim.

## Scale

One repository on one machine. Typical case is 1 to 10 conflicted files with 1 to 5 blocks each.
Every answer reads and rewrites one file, a few KB, on the UI thread: well under a frame. Nothing
here grows with the size of the repository, so nothing needs a worker or a cache.

## Design

```
 Branches view ──m──► Repository::Merge ──────────────┐
 History V / t, B, rebase continue ───────────────────┤ stops with conflicts
                                                      ▼
                                       App::OpenResolverIfStopped
                                                      │
 Status enter / operation pane enter ──► App::OpenConflict(path)
                                                      │
                                       Repository::ReadConflict ── worktree file ─► ParseConflictMarkers
                                                      │             index stages ─► exists / binary
                                                      ▼
                                       ui::ConflictPane  (o t b O T e s j k [ ] esc)
                                                      │
       ResolveHunk(path, hunk, begin, side) ◄─────────┤
       ResolveFile(path, side)  ◄─────────────────────┤
       RunEditor(+line file) ─► MarkResolved ◄────────┘
                                                      │
                                       App::AfterResolve: next block, next file, or close + how to finish
```

`src/git/conflict.cpp` owns everything that touches the repository; `src/ui/conflict_panel.cpp`
draws; App holds the open file and the block index.

## Choices and trade-offs

**Hunks come from the working-tree file's markers, not from the index's three stages.**
Solves: the user may already have edited the file, and the file is what gets rewritten, so the
two cannot disagree. Worsens: depends on the default marker format and size.
Change when: someone needs `conflict-marker-size`, which means reading `.gitattributes`.

**Whole-file answers come from the index stages.**
Solves: binary files and modify/delete conflicts have no markers to parse, and taking a side
whole should give back that side's exact bytes and executable bit. Worsens: two sources of truth
in one feature. They answer different questions, so they never compete for the same answer.

**A hunk is addressed by index plus the line it started on.**
Solves: the file is re-read before every write, and a block that moved (the user edited in another
window) is refused rather than applied to the wrong lines. This is `HunkPick::header` again.

**Staging happens automatically when the last marker goes.**
Solves: one key per block is the whole interaction, as `git mergetool` does. Worsens: a file is
staged without a separate keystroke. Guarded by refusing on an unclosed `<<<<<<<`.

**Merge has no confirm.**
It refuses on a dirty tree, `z` undoes a merge commit, and abort undoes a stopped merge. That
matches checkout. The confirm rule (destroys something unrecoverable, or visible to others) does
not apply.

**Resolver keys are letters in the overlay, not keymap actions.**
They mean something only while the pane is open, the same as the operation pane's `c` and `a`.
Movement goes through the keymap so a rebound `j`/`k` still works.

## Failure modes

| What goes wrong | What happens |
|---|---|
| File edited outside gittop between read and answer | hunk start moved, answer refused, user told to look again |
| Editor leaves a half-written block | `unclosed` set, file not staged, pane says so |
| Editor fails to start | toast names the editor and the variables to set |
| Editor exits with markers left | resolver re-reads and continues from the file as it is |
| Merge into a dirty tree | refused before anything is written |
| Merging the checked-out branch | refused |
| Unborn HEAD | refused with "make a first commit" |
| Write fails (read-only file, disk full) | `could not write <path>`, nothing staged |

## Score

Against the quality bar: requirements are explicit, every choice names its cost, and failure modes
are tested where they can be (`tests/repository_test.cpp` covers parsing, staleness, both, whole
file, modify/delete, fast-forward, clean merge and the unclosed marker).

Weakest dimension: editor integration. Only the vi family gets a line jump, and gittop trusts the
markers being gone rather than asking the editor whether the user meant it.

Current bottleneck: none that matters at this scale. If a project routinely produced hundreds of
conflicted files, the first thing to change would be re-reading the whole status after every
answer (`Apply` calls `Refresh`), which would want an incremental update of the one entry.
