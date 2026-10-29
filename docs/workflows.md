# Rebase, cherry-pick, merge and conflicts in gittop

This guide covers the operations that change commits: rebasing onto your upstream, editing commits
in place, cherry-picking, reverting, merging, resolving conflicts, and getting out again when
something stops halfway. Every key below is the default and can be rebound in `[keys]`; press `?`
in gittop to see what yours are.

## Rebase onto your upstream

Press `B` from any view. gittop replays the commits you have that your upstream does not on top of
the upstream's tip, the same as `git rebase @{upstream}`.

It refuses on a dirty working tree. Commit or stash first (`S` stashes from anywhere). It does not
stash for you, because a stash that gittop made and you did not notice is a stash you will not
think to pop.

Use this when a pull says your branch has diverged. Pull only fast-forwards, so a diverged branch
needs a rebase first, and the pull message points at `B` for that reason.

## Edit commits in place (interactive rebase)

Open the History view (`2`), put the cursor on a commit, and press:

| Key | Does |
|-----|------|
| `s` | squash into the commit below it, keeping both messages |
| `f` | fixup into the commit below it, keeping only the older message |
| `d` | drop the commit |
| `w` | reword the message |
| `J` / `K` | move the commit down / up one place |

These work on the first-parent line of HEAD and never across a merge commit. Each one asks first,
and the confirm tells you how many of the commits it rewrites are already pushed. Rewriting pushed
commits means your next push has to replace history on the remote, and gittop's push never forces,
so you would finish that from the command line.

## Cherry-pick

Cherry-picking is copy and paste. In the History view, press `c` on each commit you want to mark,
then check out the branch you want them on and press `V` there.

The marked commits are applied oldest first, whatever order you marked them in, so a run of
commits lands in the order it was written. Each one keeps its original author. Merge commits
cannot be picked.

## Revert

`t` on a commit in the History view makes a new commit that undoes it, with git's usual
`Revert "..."` message. The original commit stays in history. Use this instead of dropping a commit
that is already pushed. Merge commits cannot be reverted.

## Merge a branch

On the Branches view (`3`), put the cursor on a branch and press `m` to merge it into the branch
you have checked out. What happens depends on the two branches:

- If your branch has nothing the other lacks, gittop fast-forwards it, as `git merge` does.
- If they have both moved but touch different lines, gittop makes the merge commit with git's
  usual `Merge branch '...'` message.
- If they changed the same lines, the merge stops and the conflict resolver opens.

Like rebase, merge refuses on a dirty working tree. There is no confirm, because `z` undoes a
merge commit and abort undoes a stopped merge.

## Resolving conflicts

A merge, cherry-pick, revert or rebase stops when two changes touch the same lines. gittop opens
the resolver on the first conflicted file as soon as that happens. You can also reach it later:
`enter` on a file in the Status view's CONFLICTS column, or `enter` in the operation pane (`o`).

The resolver shows one conflicted block at a time. Your side (OURS) is on the left, the incoming
side (THEIRS) on the right, with a couple of lines of the file above and below.

| Key | Does |
|-----|------|
| `o` | keep ours for this block |
| `t` | keep theirs for this block |
| `b` | keep both, ours first |
| `O` / `T` | take ours / theirs for the whole file |
| `e` | open the file in your editor |
| `j` / `k` | next / previous block in the file |
| `]` / `[` | next / previous conflicted file |
| `s` | mark the file resolved (shown once no markers are left) |
| `esc` | close; the conflicts stay until you come back |

Each choice rewrites the file on disk and removes that block's `<<<<<<<`, `=======` and `>>>>>>>`
lines. When the last block in a file is gone, gittop stages the file and moves on to the next one.
When no conflicted files are left, the resolver closes and tells you how to finish.

During a rebase the words swap meaning. git replays your commit on top of the other branch, so
OURS is the branch you are rebasing onto and THEIRS is your own commit. The resolver says so when
a rebase is what stopped.

### Using vim or nvim

`e` opens the file in git's editor: `$GIT_EDITOR`, then `core.editor`, then `$VISUAL`, then
`$EDITOR`, then `vi`. For vi, vim and nvim, gittop passes `+<line>` so the editor opens on the block
you were looking at. To make nvim the editor for git and gittop alike:

```bash
git config --global core.editor nvim
```

When you quit the editor, gittop reads the file again. If you removed every marker, the file is
staged as resolved, which is what `git mergetool` does. If markers are left, the resolver picks up
from what the file now says.

Some conflicts have no blocks to pick from: a binary file, or a file deleted on one side and
changed on the other. For those the resolver explains which case it is, and `O` or `T` takes one
side whole. Taking the side that deleted the file deletes it.

### Finishing

- **Merge, cherry-pick or revert:** commit with `c` on the Status view. There is no continue step,
  because finishing one of these is an ordinary commit.
- **Rebase:** press `o`, then `c` to continue. gittop commits what is staged and replays the next
  commit, which may stop again and reopen the resolver.

To give up instead, press `o` and then `a` to abort. For a rebase this puts the branch back where
it started. For a merge, cherry-pick or revert it is a hard reset, which throws away everything in
the working tree, and the confirm says so.

## Undo

`z` takes back the last thing that moved HEAD, from any view: a commit, an amend, a rebase, a
reset. `Z` opens the reflog, so you can go back further than one step. If an interactive rebase
did not do what you meant, `z` is usually faster than working out how to rebuild what it changed.
